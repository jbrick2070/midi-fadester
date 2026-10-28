/*
 * Midi Fadester
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "core/takeover.h"
#include <math.h>
#include <stdio.h>

static float clamp01(float v)
{
	if (!isfinite(v) || v <= 0.0f) {
		return 0.0f;
	}
	if (v >= 1.0f) {
		return 1.0f;
	}
	return v;
}

static enum mfd_takeover_hint compute_hint(float fader_val, float target_val, float threshold)
{
	if (fabsf(target_val - fader_val) <= threshold) {
		return MFD_HINT_NONE;
	}
	return (target_val > fader_val) ? MFD_HINT_MOVE_UP : MFD_HINT_MOVE_DOWN;
}

static float compute_rescaled_effective(const struct mfd_takeover *t, float raw_clamped)
{
	if (!t) {
		return raw_clamped;
	}
	if (t->two_rail_mode) {
		if (raw_clamped <= t->lower_rescale_origin) {
			return clamp01(t->lower_rescale_val);
		}
		if (raw_clamped >= t->upper_rescale_origin) {
			return clamp01(t->upper_rescale_val);
		}
		float span = t->upper_rescale_origin - t->lower_rescale_origin;
		if (span <= 1e-5f) {
			return raw_clamped;
		}
		float u = (raw_clamped - t->lower_rescale_origin) / span;
		return clamp01(t->lower_rescale_val + u * (t->upper_rescale_val - t->lower_rescale_val));
	}
	if (!t->bidirectional_flip) {
		return raw_clamped;
	}
	if (!t->polarity_reversed) {
		float span = 1.0f - t->rescale_origin;
		if (span <= 1e-5f) {
			return 1.0f;
		}
		return clamp01((raw_clamped - t->rescale_origin) / span);
	} else {
		float span = t->rescale_origin;
		if (span <= 1e-5f) {
			return 1.0f;
		}
		return clamp01((t->rescale_origin - raw_clamped) / span);
	}
}

void mfd_takeover_init(struct mfd_takeover *t, float initial_obs_val, float pickup_threshold, bool bidirectional_flip)
{
	if (!t) {
		return;
	}
	t->state = MFD_TAKEOVER_WAITING;
	t->last_fader = 0.0f;
	t->last_raw_fader = 0.0f;
	t->target_obs = clamp01(initial_obs_val);
	t->hint = (t->target_obs >= 0.5f) ? MFD_HINT_MOVE_UP : MFD_HINT_MOVE_DOWN;
	t->pickup_threshold = (isfinite(pickup_threshold) && pickup_threshold > 0.0f) ? pickup_threshold
										      : MFD_DEFAULT_PICKUP_THRESHOLD;
	t->parked_peak = 0.0f;
	t->throw_raw_extreme = 0.0f;
	t->rescale_origin = 0.0f;
	t->lower_rescale_origin = 0.0f;
	t->upper_rescale_origin = 1.0f;
	t->lower_rescale_val = 0.0f;
	t->upper_rescale_val = 1.0f;
	t->has_last_fader = false;
	t->bidirectional_flip = bidirectional_flip;
	t->two_rail_mode = false;
	t->polarity_reversed = false;
	t->parked_at_upper_rail = false;
	t->armed_lower_rail = (t->target_obs > MFD_TWO_RAIL_LOWER_PARK_THRESHOLD);
	t->armed_upper_rail = (t->target_obs < MFD_TWO_RAIL_UPPER_PARK_THRESHOLD);
	t->dial_reversed = false;
	t->midi_at_rail_deadzone = false;
	t->has_throw_extreme = false;
}

void mfd_takeover_set_two_rail_mode(struct mfd_takeover *t, bool two_rail_mode)
{
	if (!t) {
		return;
	}
	t->two_rail_mode = two_rail_mode;
	if (two_rail_mode) {
		t->bidirectional_flip = false;
		t->polarity_reversed = false;
		t->lower_rescale_origin = 0.0f;
		t->upper_rescale_origin = 1.0f;
		t->lower_rescale_val = 0.0f;
		t->upper_rescale_val = 1.0f;
		t->armed_lower_rail = (t->target_obs > MFD_TWO_RAIL_LOWER_PARK_THRESHOLD);
		t->armed_upper_rail = (t->target_obs < MFD_TWO_RAIL_UPPER_PARK_THRESHOLD);
	}
}

void mfd_takeover_sync_external(struct mfd_takeover *t, float new_obs_val)
{
	if (!t) {
		return;
	}
	float clamped = clamp01(new_obs_val);
	if (fabsf(clamped - t->target_obs) > t->pickup_threshold) {
		t->target_obs = clamped;
		t->state = MFD_TAKEOVER_WAITING;
		t->rescale_origin = t->polarity_reversed ? 1.0f : 0.0f;
		t->lower_rescale_origin = 0.0f;
		t->upper_rescale_origin = 1.0f;
		t->lower_rescale_val = 0.0f;
		t->upper_rescale_val = 1.0f;
		t->armed_lower_rail = (clamped > MFD_TWO_RAIL_LOWER_PARK_THRESHOLD);
		t->armed_upper_rail = (clamped < MFD_TWO_RAIL_UPPER_PARK_THRESHOLD);
		t->midi_at_rail_deadzone = false;
		if (t->has_last_fader) {
			t->hint = compute_hint(t->last_fader, t->target_obs, t->pickup_threshold);
		}
	}
}

bool mfd_takeover_step(struct mfd_takeover *t, float raw_fader_norm, float *out_effective_norm)
{
	if (!t || !out_effective_norm) {
		return false;
	}

	float raw_clamped = clamp01(raw_fader_norm);

	if (t->state == MFD_TAKEOVER_PARKED) {
		t->last_raw_fader = raw_clamped;
		t->has_last_fader = true;

		if (t->parked_at_upper_rail) {
			if (raw_clamped > t->parked_peak) {
				t->parked_peak = raw_clamped;
				if (t->two_rail_mode) {
					if (raw_clamped > t->upper_rescale_origin) {
						t->upper_rescale_origin = raw_clamped;
					}
					if (raw_clamped > t->upper_rescale_val) {
						t->upper_rescale_val = raw_clamped;
					}
					float eff = compute_rescaled_effective(t, raw_clamped);
					t->last_fader = eff;
					t->target_obs = eff;
					*out_effective_norm = eff;
					return true;
				}
			}
			if ((t->parked_peak - raw_clamped) > MFD_PARKED_UNPARK_HYSTERESIS) {
				t->state = MFD_TAKEOVER_ENGAGED;
				t->hint = MFD_HINT_NONE;
				if (t->two_rail_mode) {
					t->upper_rescale_origin = raw_clamped;
					t->upper_rescale_val = t->target_obs;
					t->lower_rescale_origin = 0.0f;
					t->lower_rescale_val = 0.0f;
					t->armed_upper_rail = false;
					t->armed_lower_rail = true;
				} else if (t->bidirectional_flip) {
					t->polarity_reversed = true;
					t->rescale_origin = raw_clamped;
				} else {
					t->polarity_reversed = false;
					t->rescale_origin = 0.0f;
					t->target_obs = 0.0f;
					if (raw_clamped > (MFD_TWO_RAIL_LOWER_PARK_THRESHOLD + 1e-5f)) {
						t->state = MFD_TAKEOVER_WAITING;
						t->last_fader = raw_clamped;
						t->hint = compute_hint(raw_clamped, 0.0f, t->pickup_threshold);
						t->has_throw_extreme = false;
						*out_effective_norm = 0.0f;
						return false;
					}
				}
				float eff = compute_rescaled_effective(t, raw_clamped);
				t->last_fader = eff;
				t->target_obs = eff;
				t->throw_raw_extreme = raw_clamped;
				t->has_throw_extreme = !t->two_rail_mode;
				*out_effective_norm = eff;
				return true;
			}
		} else {
			if (raw_clamped < t->parked_peak) {
				t->parked_peak = raw_clamped;
				if (t->two_rail_mode) {
					if (raw_clamped < t->lower_rescale_origin) {
						t->lower_rescale_origin = raw_clamped;
					}
					if (raw_clamped < t->lower_rescale_val) {
						t->lower_rescale_val = raw_clamped;
					}
					float eff = compute_rescaled_effective(t, raw_clamped);
					t->last_fader = eff;
					t->target_obs = eff;
					*out_effective_norm = eff;
					return true;
				}
			}
			if ((raw_clamped - t->parked_peak) > MFD_PARKED_UNPARK_HYSTERESIS) {
				t->state = MFD_TAKEOVER_ENGAGED;
				t->hint = MFD_HINT_NONE;
				if (t->two_rail_mode) {
					t->lower_rescale_origin = raw_clamped;
					t->lower_rescale_val = t->target_obs;
					t->upper_rescale_origin = 1.0f;
					t->upper_rescale_val = 1.0f;
					t->armed_lower_rail = false;
					t->armed_upper_rail = true;
				} else if (t->bidirectional_flip) {
					t->polarity_reversed = false;
					t->rescale_origin = raw_clamped;
				} else {
					t->rescale_origin = 0.0f;
				}
				float eff = compute_rescaled_effective(t, raw_clamped);
				t->last_fader = eff;
				t->target_obs = eff;
				t->throw_raw_extreme = raw_clamped;
				t->has_throw_extreme = !t->two_rail_mode;
				*out_effective_norm = eff;
				return true;
			}
		}

		*out_effective_norm = t->target_obs;
		return false;
	}

	if (t->midi_at_rail_deadzone) {
		if (t->parked_at_upper_rail) {
			if (raw_clamped > t->parked_peak) {
				t->parked_peak = raw_clamped;
			}
			if ((t->parked_peak - raw_clamped) <= MFD_PARKED_UNPARK_HYSTERESIS) {
				t->last_raw_fader = raw_clamped;
				*out_effective_norm = t->target_obs;
				return false;
			}
		} else {
			if (raw_clamped < t->parked_peak) {
				t->parked_peak = raw_clamped;
			}
			if ((raw_clamped - t->parked_peak) <= MFD_PARKED_UNPARK_HYSTERESIS) {
				t->last_raw_fader = raw_clamped;
				*out_effective_norm = t->target_obs;
				return false;
			}
		}
		t->midi_at_rail_deadzone = false;
	}

	if (t->two_rail_mode && !t->has_last_fader) {
		if (raw_clamped <= MFD_TWO_RAIL_LOWER_PARK_THRESHOLD + 1e-5f) {
			t->armed_lower_rail = false;
			t->armed_upper_rail = true;
		} else if (raw_clamped >= MFD_TWO_RAIL_UPPER_PARK_THRESHOLD - 1e-5f) {
			t->armed_upper_rail = false;
			t->armed_lower_rail = true;
		} else {
			t->armed_lower_rail = true;
			t->armed_upper_rail = true;
		}
	}

	if (t->two_rail_mode && t->state == MFD_TAKEOVER_ENGAGED) {
		if (raw_clamped > t->upper_rescale_origin) {
			t->upper_rescale_origin = raw_clamped;
			if (raw_clamped > t->upper_rescale_val) {
				t->upper_rescale_val = raw_clamped;
			}
			if (raw_clamped >= MFD_TWO_RAIL_UPPER_PARK_THRESHOLD - 1e-5f) {
				t->armed_upper_rail = true;
			}
		}
		if (raw_clamped < t->lower_rescale_origin) {
			t->lower_rescale_origin = raw_clamped;
			if (raw_clamped < t->lower_rescale_val) {
				t->lower_rescale_val = raw_clamped;
			}
			if (raw_clamped <= MFD_TWO_RAIL_LOWER_PARK_THRESHOLD + 1e-5f) {
				t->armed_lower_rail = true;
			}
		}
	}

	if (!t->two_rail_mode && t->bidirectional_flip && !t->has_last_fader && t->target_obs <= 1e-5f &&
	    raw_clamped >= (MFD_TWO_RAIL_UPPER_PARK_THRESHOLD - 1e-5f)) {
		t->polarity_reversed = true;
		t->rescale_origin = 1.0f;
	}

	float effective = compute_rescaled_effective(t, raw_clamped);

	if (t->state == MFD_TAKEOVER_WAITING) {
		bool within = (fabsf(effective - t->target_obs) <= t->pickup_threshold) ||
			      (!t->two_rail_mode && t->target_obs <= 1e-5f &&
			       effective <= (MFD_TWO_RAIL_LOWER_PARK_THRESHOLD + 1e-5f));
		bool crossed = false;
		if (t->has_last_fader) {
			crossed = ((t->last_fader <= t->target_obs && effective >= t->target_obs) ||
				   (t->last_fader >= t->target_obs && effective <= t->target_obs));
		}

		t->last_fader = effective;
		t->last_raw_fader = raw_clamped;
		t->has_last_fader = true;

		if (!within && !crossed) {
			t->hint = compute_hint(effective, t->target_obs, t->pickup_threshold);
			*out_effective_norm = t->target_obs;
			return false;
		}
		t->state = MFD_TAKEOVER_ENGAGED;
	}

	if (t->two_rail_mode) {
		if (effective > MFD_TWO_RAIL_LOWER_PARK_THRESHOLD + 1e-5f) {
			t->armed_lower_rail = true;
		}
		if (effective < MFD_TWO_RAIL_UPPER_PARK_THRESHOLD - 1e-5f) {
			t->armed_upper_rail = true;
		}
		if (t->armed_upper_rail && effective >= MFD_TWO_RAIL_UPPER_PARK_THRESHOLD - 1e-5f) {
			t->state = MFD_TAKEOVER_PARKED;
			t->parked_at_upper_rail = true;
			t->parked_peak = raw_clamped;
			t->armed_upper_rail = false;
			t->armed_lower_rail = true;
		} else if (t->armed_lower_rail && effective <= MFD_TWO_RAIL_LOWER_PARK_THRESHOLD + 1e-5f) {
			t->state = MFD_TAKEOVER_PARKED;
			t->parked_at_upper_rail = false;
			t->parked_peak = raw_clamped;
			t->armed_lower_rail = false;
			t->armed_upper_rail = true;
		}
	}

	if (!t->two_rail_mode) {
		bool toward_upper = t->bidirectional_flip ? !t->polarity_reversed : true;
		if (!t->has_throw_extreme) {
			t->throw_raw_extreme = raw_clamped;
			t->has_throw_extreme = true;
		} else if (toward_upper && raw_clamped > t->throw_raw_extreme) {
			t->throw_raw_extreme = raw_clamped;
		} else if (!toward_upper && raw_clamped < t->throw_raw_extreme) {
			t->throw_raw_extreme = raw_clamped;
		}
	}

	t->last_fader = effective;
	t->last_raw_fader = raw_clamped;
	t->has_last_fader = true;
	t->target_obs = effective;
	t->hint = MFD_HINT_NONE;
	*out_effective_norm = effective;
	return true;
}

void mfd_takeover_notify_tbar_completed(struct mfd_takeover *t)
{
	if (!t) {
		return;
	}
	t->parked_at_upper_rail = t->bidirectional_flip ? !t->polarity_reversed : true;
	if (t->has_throw_extreme) {
		t->parked_peak = t->throw_raw_extreme;
	} else {
		t->parked_peak = t->has_last_fader ? t->last_raw_fader : (t->parked_at_upper_rail ? 1.0f : 0.0f);
	}
	t->has_throw_extreme = false;
	t->target_obs = 0.0f;
	t->last_fader = 0.0f;
	t->state = MFD_TAKEOVER_PARKED;
	t->hint = MFD_HINT_NONE;
	t->midi_at_rail_deadzone = false;
}

void mfd_takeover_notify_rail_completed(struct mfd_takeover *t, bool upper_rail, float applied_val)
{
	if (!t) {
		return;
	}
	float clamped = clamp01(applied_val);
	t->parked_at_upper_rail = upper_rail;
	t->parked_peak = t->has_last_fader ? t->last_raw_fader : clamped;
	t->target_obs = clamped;
	t->last_fader = clamped;
	t->armed_upper_rail = !upper_rail;
	t->armed_lower_rail = upper_rail;
	t->state = MFD_TAKEOVER_PARKED;
	t->hint = MFD_HINT_NONE;
	t->midi_at_rail_deadzone = false;
}

void mfd_takeover_unpark_from_api(struct mfd_takeover *t, float api_val)
{
	if (!t) {
		return;
	}
	float clamped = clamp01(api_val);
	bool was_parked = (t->state == MFD_TAKEOVER_PARKED);
	if (was_parked && t->has_last_fader) {
		t->midi_at_rail_deadzone = true;
	}

	if (t->two_rail_mode) {
		t->lower_rescale_origin = 0.0f;
		t->upper_rescale_origin = 1.0f;
		t->lower_rescale_val = 0.0f;
		t->upper_rescale_val = 1.0f;
		if (was_parked) {
			t->armed_upper_rail = !t->parked_at_upper_rail ||
					      (clamped < MFD_TWO_RAIL_UPPER_PARK_THRESHOLD - 1e-5f);
			t->armed_lower_rail = t->parked_at_upper_rail ||
					      (clamped > MFD_TWO_RAIL_LOWER_PARK_THRESHOLD + 1e-5f);
		} else {
			if (clamped > MFD_TWO_RAIL_LOWER_PARK_THRESHOLD + 1e-5f) {
				t->armed_lower_rail = true;
			}
			if (clamped < MFD_TWO_RAIL_UPPER_PARK_THRESHOLD - 1e-5f) {
				t->armed_upper_rail = true;
			}
		}
	} else if (t->bidirectional_flip) {
		if (was_parked) {
			t->polarity_reversed = t->parked_at_upper_rail;
			t->rescale_origin = t->polarity_reversed ? 1.0f : 0.0f;
		}
	}

	t->target_obs = clamped;
	if (t->has_last_fader) {
		float anchor_raw = t->midi_at_rail_deadzone ? t->parked_peak : t->last_raw_fader;
		float fader_eff = compute_rescaled_effective(t, anchor_raw);
		t->last_fader = fader_eff;
		t->state = MFD_TAKEOVER_WAITING;
		t->hint = compute_hint(fader_eff, clamped, t->pickup_threshold);
	} else {
		t->midi_at_rail_deadzone = false;
		t->last_fader = clamped;
		t->state = MFD_TAKEOVER_ENGAGED;
		t->hint = MFD_HINT_NONE;
	}
}

float mfd_takeover_throw_to_mix_value(const struct mfd_takeover *t, float throw_progress)
{
	float p = clamp01(throw_progress);
	if (t && t->polarity_reversed) {
		return 1.0f - p;
	}
	return p;
}

float mfd_takeover_mix_to_throw_progress(const struct mfd_takeover *t, float mix_value)
{
	float m = clamp01(mix_value);
	if (t && t->polarity_reversed) {
		return 1.0f - m;
	}
	return m;
}

int mfd_takeover_format_output_bar(const struct mfd_takeover *t, enum mfd_target_type target_type, float applied_val,
				   float alpha_a, float alpha_b, float db_val, char *out_text, size_t text_size)
{
	int pct = (int)lroundf(clamp01(applied_val) * 100.0f);
	if (!out_text || text_size == 0U) {
		return pct;
	}

	if (t && t->state == MFD_TAKEOVER_WAITING) {
		int tgt_pct = (int)lroundf(clamp01(t->target_obs) * 100.0f);
		const char *arrow = (t->hint == MFD_HINT_MOVE_UP) ? "\xE2\x86\x91" : "\xE2\x86\x93";
		snprintf(out_text, text_size, "%s %d%%", arrow, tgt_pct);
		return tgt_pct;
	}

	if (target_type == MFD_TARGET_TWO_SOURCE_MIX) {
		int a_pct = (int)lroundf(clamp01(alpha_a) * 100.0f);
		int b_pct = (int)lroundf(clamp01(alpha_b) * 100.0f);
		snprintf(out_text, text_size, "%d%% (A:%d%% B:%d%%)", pct, a_pct, b_pct);
	} else if (target_type == MFD_TARGET_AUDIO_VOLUME) {
		if (isfinite(db_val)) {
			snprintf(out_text, text_size, "%.1f dB (%d%%)", (double)db_val, pct);
		} else {
			snprintf(out_text, text_size, "-inf dB (0%%)");
		}
	} else {
		snprintf(out_text, text_size, "%d%%", pct);
	}

	return pct;
}
