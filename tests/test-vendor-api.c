/*
 * Midi Fadester — test-vendor-api
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "obs/mfd-vendor-api.h"
#include "core/mixer-math.h"
#include <obs-module.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("midi-fadester", "en-US")

#define REQUIRE(cond)                                                                   \
	do {                                                                            \
		if (!(cond)) {                                                          \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			return 1;                                                       \
		}                                                                       \
	} while (0)

static int g_mock_tbar_calls = 0;
static float g_mock_tbar_last_val = -1.0f;

static bool mock_tbar_driver(float t_norm, bool *out_completed_throw, char *out_err, size_t err_size)
{
	if (out_err && err_size > 0U) {
		out_err[0] = '\0';
	}
	++g_mock_tbar_calls;
	g_mock_tbar_last_val = t_norm;
	if (out_completed_throw) {
		*out_completed_throw = (t_norm >= 0.999f);
	}
	return true;
}

int main(void)
{
	struct mfd_engine eng;
	memset(&eng, 0, sizeof(eng));
	eng.smooth_tau_ms = 12.0f;
	eng.tbar_bidirectional_flip = true;
	eng.tbar_driver = mock_tbar_driver;

	/* 1. load_apc40_defaults via vendor API */
	obs_data_t *req = obs_data_create();
	obs_data_t *res = obs_data_create();
	mfd_vendor_handle_load_apc40_defaults(req, res, &eng);
	REQUIRE(obs_data_get_bool(res, "success"));
	REQUIRE(eng.fader_slots[0].target_type == MFD_TARGET_TBAR);
	REQUIRE(eng.fader_slots[1].target_type == MFD_TARGET_UNASSIGNED);
	obs_data_release(req);
	obs_data_release(res);

	/* 2. set_slot via vendor API: configure Slot 1 for Two-Source Dip-Free Crossfade */
	req = obs_data_create();
	res = obs_data_create();
	obs_data_set_int(req, "slot", 1);
	obs_data_set_bool(req, "enabled", true);
	obs_data_set_int(req, "target_type", (int)MFD_TARGET_TWO_SOURCE_MIX);
	obs_data_set_int(req, "curve", (int)MFD_CURVE_DIP_FREE);
	obs_data_set_string(req, "source_a", "Cam_A");
	obs_data_set_string(req, "source_b", "Cam_B");
	mfd_vendor_handle_set_slot(req, res, &eng);
	REQUIRE(obs_data_get_bool(res, "success"));
	REQUIRE(eng.fader_slots[1].target_type == MFD_TARGET_TWO_SOURCE_MIX);
	REQUIRE(strcmp(eng.fader_slots[1].source_a_name, "Cam_A") == 0);
	REQUIRE(strcmp(eng.fader_slots[1].source_b_name, "Cam_B") == 0);
	obs_data_release(req);
	obs_data_release(res);

	/* 3. set_fader_value via vendor API: drive Two-Source Crossfade to 0.5 -> alpha_a=1.0, alpha_b=0.5 */
	req = obs_data_create();
	res = obs_data_create();
	obs_data_set_int(req, "slot", 1);
	obs_data_set_double(req, "value", 0.5);
	mfd_vendor_handle_set_fader_value(req, res, &eng);
	REQUIRE(obs_data_get_bool(res, "success"));
	REQUIRE(fabs(obs_data_get_double(res, "alpha_a") - 1.0) < 1e-5);
	REQUIRE(fabs(obs_data_get_double(res, "alpha_b") - 0.5) < 1e-5);
	obs_data_release(req);
	obs_data_release(res);

	/* 4. set_fader_value via vendor API: drive T-Bar (Slot 0) through 0.5 -> 1.0 throw */
	req = obs_data_create();
	res = obs_data_create();
	obs_data_set_int(req, "slot", 0);
	obs_data_set_double(req, "value", 0.5);
	mfd_vendor_handle_set_fader_value(req, res, &eng);
	REQUIRE(obs_data_get_bool(res, "success"));
	REQUIRE(g_mock_tbar_calls == 1);
	REQUIRE(fabsf(g_mock_tbar_last_val - 0.5f) < 1e-5f);
	obs_data_release(req);
	obs_data_release(res);

	req = obs_data_create();
	res = obs_data_create();
	obs_data_set_int(req, "slot", 0);
	obs_data_set_double(req, "value", 1.0);
	mfd_vendor_handle_set_fader_value(req, res, &eng);
	REQUIRE(obs_data_get_bool(res, "success"));
	REQUIRE(g_mock_tbar_calls == 2);
	REQUIRE(fabsf(g_mock_tbar_last_val - 1.0f) < 1e-5f);
	/* Completion resets T-bar applied value to 0.0 and flips polarity */
	REQUIRE(fabs(obs_data_get_double(res, "applied_value") - 0.0) < 1e-5);
	REQUIRE(eng.fader_slots[0].takeover.polarity_reversed);
	obs_data_release(req);
	obs_data_release(res);

	/* 5. get_state returns full slot array */
	req = obs_data_create();
	res = obs_data_create();
	mfd_vendor_handle_get_state(req, res, &eng);
	REQUIRE(obs_data_get_bool(res, "success"));
	obs_data_array_t *arr = obs_data_get_array(res, "slots");
	REQUIRE(arr != NULL);
	REQUIRE(obs_data_array_count(arr) == MFD_MAX_FADER_SLOTS);
	obs_data_array_release(arr);
	obs_data_release(req);
	obs_data_release(res);

	printf("test-vendor-api: PASS\n");
	return 0;
}
