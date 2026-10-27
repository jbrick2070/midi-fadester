/*
 * Visual Sidechain - Pure-C Modulation Slot Math & Rate/Epsilon Gate
 * Copyright (C) 2026
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

	const double amount = clamp_d(cfg->amount, -1.0, 1.0);
	const double min_v = cfg->min_value;
	const double max_v = cfg->max_value;
	const double span = max_v - min_v;

	double mapped = min_v;
	if (amount >= 0.0) {
		mapped = min_v + (sig * amount) * span;
	} else {
		mapped = max_v - (sig * (-amount)) * span;
	}

	const double lo = (min_v <= max_v) ? min_v : max_v;
	const double hi = (min_v <= max_v) ? max_v : min_v;
	mapped = clamp_d(mapped, lo, hi);

	if (cfg->prop_type == VS_PROP_INT) {
		const int64_t step = (cfg->int_step > 1) ? cfg->int_step : 1;
		const int64_t i_lo = (int64_t)llround(lo);
		const int64_t i_hi = (int64_t)llround(hi);
		int64_t quantized = i_lo + llround((mapped - (double)i_lo) / (double)step) * step;
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
	out_res->int_val = (int64_t)llround(mapped);

	if (state->has_last_written && fabs(mapped - state->last_written_float) < VS_MOD_FLOAT_EPSILON) {
		out_res->should_write = false;
		return;
	}

	state->last_written_float = mapped;
	state->has_last_written = true;
	out_res->should_write = true;
}
