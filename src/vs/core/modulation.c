/*
 * Visual Sidechain - Pure-C Modulation Slot Math & Rate/Epsilon Gate
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "modulation.h"

#include <math.h>
#include <string.h>

static inline bool is_finite_f64(double v)
{
	uint64_t bits = 0;
	memcpy(&bits, &v, sizeof(bits));
	return (bits & 0x7ff0000000000000ULL) != 0x7ff0000000000000ULL;
}

static double clamp_d(double v, double lo, double hi)
{
	if (!is_finite_f64(v))
		return lo;
	if (v < lo)
		return lo;
	if (v > hi)
		return hi;
	return v;
}

static int64_t saturating_round_i64(double value)
{
	if (!is_finite_f64(value))
		return 0;
	if (value >= (double)INT64_MAX)
		return INT64_MAX;
	if (value <= (double)INT64_MIN)
		return INT64_MIN;
	return (int64_t)llround(value);
}

void vs_mod_slot_default_config(struct vs_mod_slot_config *cfg)
{
	if (!cfg)
		return;
	memset(cfg, 0, sizeof(*cfg));
	cfg->enabled = false;
	cfg->prop_type = VS_PROP_FLOAT;
	cfg->signal_id = VS_SIG_BASS;
	cfg->custom_low_hz = 60.0f;
	cfg->custom_high_hz = 120.0f;
	cfg->custom_mode = VS_HZ_MODE_PEAK;
	cfg->amount = 1.0;
	cfg->depth_fader_slot = -1;
	cfg->depth_scale = 1.0;
	cfg->min_value = 0.0;
	cfg->max_value = 1.0;
	cfg->int_step = 1;
	cfg->invert = false;
	cfg->rate_divider = 1u;
}

void vs_mod_slot_reset_state(struct vs_mod_slot_state *state)
{
	if (!state)
		return;
	memset(state, 0, sizeof(*state));
}

bool vs_mod_slot_has_duplicate_target(const struct vs_mod_slot_config *cfgs, size_t earlier_count,
				      const struct vs_mod_slot_config *candidate)
{
	if (!cfgs || !candidate || !candidate->enabled || candidate->target_filter[0] == '\0' ||
	    candidate->target_setting[0] == '\0')
		return false;
	if (earlier_count > VS_MOD_MAX_SLOTS)
		earlier_count = VS_MOD_MAX_SLOTS;
	for (size_t i = 0; i < earlier_count; i++) {
		if (cfgs[i].enabled && strcmp(cfgs[i].target_filter, candidate->target_filter) == 0 &&
		    strcmp(cfgs[i].target_setting, candidate->target_setting) == 0)
			return true;
	}
	return false;
}

void vs_mod_slot_eval(const struct vs_mod_slot_config *cfg, struct vs_mod_slot_state *state, float signal_01,
		      struct vs_mod_eval_result *out_res)
{
	if (!cfg || !state || !out_res)
		return;

	memset(out_res, 0, sizeof(*out_res));
	out_res->prop_type = cfg->prop_type;

	if (!cfg->enabled || cfg->prop_type == VS_PROP_NONE || cfg->target_filter[0] == '\0' ||
	    cfg->target_setting[0] == '\0') {
		return;
	}

	uint32_t div = cfg->rate_divider;
	if (div == 0u)
		div = 1u;

	const uint32_t tick = state->tick_counter++;
	if ((tick % div) != 0u) {
		return;
	}

	double sig = clamp_d((double)signal_01, 0.0, 1.0);
	if (cfg->invert)
		sig = 1.0 - sig;

	const double raw_amount = clamp_d(cfg->amount, -1.0, 1.0);
	const double depth = clamp_d(cfg->depth_scale, 0.0, 1.0);
	const double min_v = is_finite_f64(cfg->min_value) ? cfg->min_value : 0.0;
	const double max_v = is_finite_f64(cfg->max_value) ? cfg->max_value : 1.0;
	const double weight = raw_amount >= 0.0 ? sig * raw_amount * depth : 1.0 - sig * (-raw_amount) * depth;
	/* Weighted interpolation avoids overflowing max-min for large finite, opposite-signed bounds. */
	double mapped = (1.0 - weight) * min_v + weight * max_v;

	const double lo = (min_v <= max_v) ? min_v : max_v;
	const double hi = (min_v <= max_v) ? max_v : min_v;
	mapped = clamp_d(mapped, lo, hi);

	if (cfg->prop_type == VS_PROP_INT) {
		const int64_t step = (cfg->int_step > 1) ? cfg->int_step : 1;
		const int64_t i_lo = saturating_round_i64(lo);
		const int64_t i_hi = saturating_round_i64(hi);
		mapped = clamp_d(mapped, (double)i_lo, (double)i_hi);
		/* Keep the step calculation in double: the full signed range exceeds INT64_MAX. */
		const double stepped = (double)i_lo + round((mapped - (double)i_lo) / (double)step) * (double)step;
		int64_t quantized = saturating_round_i64(stepped);
		if (quantized < i_lo)
			quantized = i_lo;
		if (quantized > i_hi)
			quantized = i_hi;

		out_res->int_val = quantized;
		out_res->float_val = (double)quantized;

		if (state->has_last_written && state->last_written_int == quantized) {
			out_res->should_write = false;
			return;
		}
		state->last_written_int = quantized;
		state->last_written_float = (double)quantized;
		state->has_last_written = true;
		out_res->should_write = true;
		return;
	}

	out_res->float_val = mapped;
	out_res->int_val = saturating_round_i64(mapped);

	if (state->has_last_written && fabs(mapped - state->last_written_float) < VS_MOD_FLOAT_EPSILON) {
		out_res->should_write = false;
		return;
	}

	state->last_written_float = mapped;
	state->has_last_written = true;
	out_res->should_write = true;
}

size_t vs_mod_eval_and_batch_slots(const struct vs_mod_slot_config *cfgs, struct vs_mod_slot_state *states,
				   const float *signals_01, size_t slot_count,
				   struct vs_mod_target_batch out_batches[VS_MOD_MAX_SLOTS])
{
	if (!cfgs || !states || !signals_01 || !out_batches)
		return 0u;

	if (slot_count > VS_MOD_MAX_SLOTS)
		slot_count = VS_MOD_MAX_SLOTS;

	size_t batch_count = 0u;
	memset(out_batches, 0, sizeof(struct vs_mod_target_batch) * VS_MOD_MAX_SLOTS);

	for (size_t i = 0u; i < slot_count; i++) {
		if (vs_mod_slot_has_duplicate_target(cfgs, i, &cfgs[i]))
			continue;
		struct vs_mod_eval_result eval;
		vs_mod_slot_eval(&cfgs[i], &states[i], signals_01[i], &eval);
		if (!eval.should_write)
			continue;

		size_t b = 0u;
		while (b < batch_count) {
			if (strcmp(out_batches[b].target_filter, cfgs[i].target_filter) == 0)
				break;
			b++;
		}
		if (b == batch_count) {
			out_batches[b].target_filter = cfgs[i].target_filter;
			out_batches[b].write_count = 0u;
			batch_count++;
		}

		struct vs_mod_target_write *w = &out_batches[b].writes[out_batches[b].write_count++];
		w->slot_index = (uint32_t)i;
		w->target_setting = cfgs[i].target_setting;
		w->prop_type = cfgs[i].prop_type;
		w->float_val = eval.float_val;
		w->int_val = eval.int_val;
	}

	return batch_count;
}
