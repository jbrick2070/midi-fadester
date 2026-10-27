/*
 * Midi Fadester
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "obs/mfd-tbar.h"
#include "obs/mfd-engine.h"
#include "obs/mfd-layer-filter.h"
#include "obs/mfd-vendor-api.h"
#include "midi/winmm-device.h"
#include "core/tbar-logic.h"
#include "core/mixer-math.h"

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/bmem.h>
#include <cassert>
#include <cmath>
#include <cstring>
#include "obs/obs-websocket-api.h"
#include "plugin-support.h"

#include <QApplication>
#include <QByteArray>
#include <QColor>
#include <QFileInfo>
#include <QImage>
#include <QMetaObject>
#include <QPointer>
#include <QProcess>
#include <QSlider>
#include <QThread>
#include <QVariant>
#include <QWidget>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

static QPointer<QSlider> g_tbar_slider;
static std::atomic<bool> g_studio_mode_active{false};
static std::atomic<bool> g_slider_tbar_found{false};
static std::atomic<bool> g_logged_missing_error{false};
static std::atomic<int> g_last_pos_1024{0};
static std::atomic<int> g_tbar_set_count{0};
static std::atomic<int> g_tbar_release_count{0};
static std::atomic<int> g_tbar_writes_since_release{0};
static std::atomic<int> g_tbar_writes_before_unpark{0};
static std::atomic<bool> g_tbar_awaiting_unpark{false};

static QSlider *locate_slider_tbar_on_ui_thread(void)
{
	QWidget *main_win = static_cast<QWidget *>(obs_frontend_get_main_window());
	if (!main_win) {
		return nullptr;
	}

	const QList<QSlider *> sliders = main_win->findChildren<QSlider *>();
	for (QSlider *s : sliders) {
		if (!s) {
			continue;
		}
		const QVariant cls = s->property("class");
		if (cls.isValid() && cls.toString() == QStringLiteral("slider-tbar")) {
			return s;
		}
	}
	return nullptr;
}

static void refresh_studio_mode_state_on_ui_thread(void)
{
	bool active = obs_frontend_preview_program_mode_active();
	g_studio_mode_active.store(active, std::memory_order_relaxed);

	if (!active) {
		g_tbar_slider.clear();
		g_slider_tbar_found.store(false, std::memory_order_relaxed);
		g_logged_missing_error.store(false, std::memory_order_relaxed);
		g_last_pos_1024.store(0, std::memory_order_relaxed);
		return;
	}

	QSlider *slider = locate_slider_tbar_on_ui_thread();
	g_tbar_slider = slider;
	if (slider) {
		g_slider_tbar_found.store(true, std::memory_order_relaxed);
		g_logged_missing_error.store(false, std::memory_order_relaxed);
		obs_log(LOG_INFO, "Studio Mode T-bar QSlider ('slider-tbar') resolved (range %d..%d)",
			slider->minimum(), slider->maximum());
	} else {
		g_slider_tbar_found.store(false, std::memory_order_relaxed);
		if (!g_logged_missing_error.exchange(true, std::memory_order_relaxed)) {
			obs_log(LOG_ERROR,
				"CRITICAL: Studio Mode is active, but Qt QSlider with property class='slider-tbar' was not found in OBS main window! T-bar sync disabled.");
		}
	}
}

static void execute_tbar_on_ui_thread(int pos_1024, bool do_set, bool do_release)
{
	if (!obs_frontend_preview_program_mode_active()) {
		g_studio_mode_active.store(false, std::memory_order_relaxed);
		g_tbar_slider.clear();
		g_slider_tbar_found.store(false, std::memory_order_relaxed);
		return;
	}

	if (g_tbar_slider.isNull()) {
		g_tbar_slider = locate_slider_tbar_on_ui_thread();
		g_slider_tbar_found.store(!g_tbar_slider.isNull(), std::memory_order_relaxed);
	}

	if (g_tbar_slider.isNull()) {
		if (!g_logged_missing_error.exchange(true, std::memory_order_relaxed)) {
			obs_log(LOG_ERROR,
				"CRITICAL: Studio Mode 'slider-tbar' QSlider missing during T-bar throw! Aborting T-bar update.");
		}
		return;
	}

	if (do_set) {
		/*
		 * Sync both OBSBasic::tBar QSlider value and OBS transition position so that
		 * obs_frontend_release_tbar() sees tBar->value() >= 921 at completion and does not
		 * snap back to the old scene (OBSStudioAPI.cpp:115 vs OBSBasic_Transitions.cpp:752).
		 */
		g_tbar_slider->blockSignals(true);
		g_tbar_slider->setValue(pos_1024);
		g_tbar_slider->blockSignals(false);
		obs_frontend_set_tbar_position(pos_1024);
		g_tbar_set_count.fetch_add(1, std::memory_order_relaxed);
		g_tbar_writes_since_release.fetch_add(1, std::memory_order_relaxed);
	}

	if (do_release) {
		obs_frontend_release_tbar();
		g_tbar_release_count.fetch_add(1, std::memory_order_relaxed);
		g_tbar_writes_since_release.store(0, std::memory_order_relaxed);
	}
}

#ifdef MFD_ENABLE_SELF_TEST
static obs_data_t *call_real_ws_vendor_request_for_vendor(const char *vendor_name, const char *request_type,
							  obs_data_t *vendor_req_data)
{
	obs_data_t *outer = obs_data_create();
	obs_data_set_string(outer, "vendorName", vendor_name);
	obs_data_set_string(outer, "requestType", request_type);
	if (vendor_req_data) {
		obs_data_set_obj(outer, "requestData", vendor_req_data);
	}

	struct obs_websocket_request_response *resp = obs_websocket_call_request("CallVendorRequest", outer);
	obs_data_release(outer);

	if (!resp) {
		return nullptr;
	}
	obs_data_t *parsed = nullptr;
	if (resp->status_code == 100 && resp->response_data) {
		obs_data_t *full = obs_data_create_from_json(resp->response_data);
		if (full) {
			parsed = obs_data_get_obj(full, "responseData");
			obs_data_release(full);
		}
	}
	obs_websocket_request_response_free(resp);
	return parsed;
}

static obs_data_t *call_real_ws_vendor_request(const char *request_type, obs_data_t *vendor_req_data)
{
	return call_real_ws_vendor_request_for_vendor(MFD_VENDOR_NAME, request_type, vendor_req_data);
}

static double srgb_nonlinear_to_linear_channel(double u)
{
	return (u <= 0.04045) ? (u / 12.92) : std::pow((u + 0.055) / 1.055, 2.4);
}

static bool capture_source_screenshot_avg_rgb(const char *source_name, float *out_r, float *out_g, float *out_b,
					      float *out_srgb_brightness)
{
	obs_data_t *req = obs_data_create();
	obs_data_set_string(req, "sourceName", source_name);
	obs_data_set_string(req, "imageFormat", "bmp");
	obs_data_set_int(req, "imageWidth", 64);
	obs_data_set_int(req, "imageHeight", 64);

	struct obs_websocket_request_response *resp = obs_websocket_call_request("GetSourceScreenshot", req);
	obs_data_release(req);

	if (!resp) {
		obs_log(LOG_ERROR, "GetSourceScreenshot returned null response for '%s'", source_name);
		return false;
	}

	bool ok = false;
	if (resp->status_code == 100 && resp->response_data) {
		obs_data_t *full = obs_data_create_from_json(resp->response_data);
		if (full) {
			const char *img_data = obs_data_get_string(full, "imageData");
			obs_data_t *rdata = nullptr;
			if (!img_data || img_data[0] == '\0') {
				rdata = obs_data_get_obj(full, "responseData");
				if (rdata) {
					img_data = obs_data_get_string(rdata, "imageData");
				}
			}
			const char *comma = img_data ? std::strchr(img_data, ',') : nullptr;
			if (comma && comma[1] != '\0') {
				QByteArray b64(comma + 1);
				QByteArray raw = QByteArray::fromBase64(b64);
				QImage img = QImage::fromData(raw, "BMP");
				if (!img.isNull() && img.width() > 0 && img.height() > 0) {
					double sum_r = 0.0;
					double sum_g = 0.0;
					double sum_b = 0.0;
					double sum_srgb = 0.0;
					const int w = img.width();
					const int h = img.height();
					for (int y = 0; y < h; ++y) {
						for (int x = 0; x < w; ++x) {
							const QColor c = img.pixelColor(x, y);
							sum_r += srgb_nonlinear_to_linear_channel(c.redF());
							sum_g += srgb_nonlinear_to_linear_channel(c.greenF());
							sum_b += srgb_nonlinear_to_linear_channel(c.blueF());
							sum_srgb += c.redF() + c.greenF() + c.blueF();
						}
					}
					const double total = static_cast<double>(w) * static_cast<double>(h);
					*out_r = static_cast<float>(sum_r / total);
					*out_g = static_cast<float>(sum_g / total);
					*out_b = static_cast<float>(sum_b / total);
					if (out_srgb_brightness) {
						*out_srgb_brightness = static_cast<float>(sum_srgb / total);
					}
					ok = true;
				}
			}
			if (rdata) {
				obs_data_release(rdata);
			}
			obs_data_release(full);
		}
	} else {
		obs_log(LOG_ERROR, "GetSourceScreenshot failed for '%s': status=%u comment=%s", source_name,
			resp->status_code, resp->comment ? resp->comment : "");
	}

	obs_websocket_request_response_free(resp);
	return ok;
}

struct selftest_sig_capture {
	int count;
	char slot[32];
	float value;
	char origin[32];
};

static void on_selftest_fader_signal(void *data, calldata_t *cd)
{
	auto *cap = static_cast<selftest_sig_capture *>(data);
	if (!cap || !cd) {
		return;
	}
	++cap->count;
	const char *s = calldata_string(cd, "slot");
	const char *o = calldata_string(cd, "origin");
	std::snprintf(cap->slot, sizeof(cap->slot), "%s", s ? s : "");
	std::snprintf(cap->origin, sizeof(cap->origin), "%s", o ? o : "");
	cap->value = static_cast<float>(calldata_float(cd, "value"));
}

static log_handler_t g_orig_log_handler = nullptr;
static void *g_orig_log_param = nullptr;
static std::atomic<int> g_obs_manual_started{0};
static std::atomic<int> g_obs_manual_finished{0};
static std::atomic<int> g_obs_manual_cancelled{0};

static void selftest_log_handler(int lvl, const char *msg, va_list args, void *p)
{
	if (msg) {
		char formatted[512];
		va_list copy;
		va_copy(copy, args);
		std::vsnprintf(formatted, sizeof(formatted), msg, copy);
		va_end(copy);

		if (std::strstr(formatted, "User started manual transition to scene ") != nullptr) {
			g_obs_manual_started.fetch_add(1, std::memory_order_relaxed);
		} else if (std::strstr(formatted, "Manual transition to scene ") != nullptr &&
			   std::strstr(formatted, " finished") != nullptr) {
			g_obs_manual_finished.fetch_add(1, std::memory_order_relaxed);
		} else if (std::strstr(formatted, "Manual transition cancelled") != nullptr) {
			g_obs_manual_cancelled.fetch_add(1, std::memory_order_relaxed);
		}
	}
	if (g_orig_log_handler) {
		g_orig_log_handler(lvl, msg, args, g_orig_log_param);
	} else {
		(void)p;
	}
}

static void run_live_selftest_worker(void)
{
	std::this_thread::sleep_for(std::chrono::milliseconds(300));
	obs_log(LOG_INFO, "Running live OBS 33 vendor API & integration self-test (MFD_SELF_TEST=1)...");

	base_get_log_handler(&g_orig_log_handler, &g_orig_log_param);
	base_set_log_handler(selftest_log_handler, nullptr);

	QWidget *main_win = static_cast<QWidget *>(obs_frontend_get_main_window());
	struct mfd_engine *eng = mfd_engine_get();

	bool was_studio = false;
	obs_source_t *prev_scene = nullptr;
	obs_scene_t *test_scene = nullptr;
	obs_scene_t *test_scene_b = nullptr;
	obs_source_t *src_a = nullptr;
	obs_source_t *src_b = nullptr;

	char src_a_actual_name[128] = "__mfd_selftest_source_a__";
	char src_b_actual_name[128] = "__mfd_selftest_source_b__";

	QMetaObject::invokeMethod(
		main_win,
		[&]() {
			was_studio = obs_frontend_preview_program_mode_active();
			if (was_studio) {
				obs_frontend_set_preview_program_mode(false);
				refresh_studio_mode_state_on_ui_thread();
			}

			/* Remove any leftover self-test sources/scenes from prior interrupted runs */
			const char *stale_names[] = {
				"__mfd_selftest_source_a__",   "__mfd_selftest_source_b__",
				"__mfd_selftest_source_a__ 2", "__mfd_selftest_source_b__ 2",
				"__mfd_selftest_scene__",      "__mfd_selftest_scene__ 2",
				"__mfd_selftest_scene_b__",    "__mfd_selftest_scene_b__ 2",
			};
			for (const char *sn : stale_names) {
				obs_source_t *old = obs_get_source_by_name(sn);
				if (old) {
					obs_source_remove(old);
					obs_source_release(old);
				}
			}

			prev_scene = obs_frontend_get_current_scene();

			test_scene = obs_scene_create("__mfd_selftest_scene__");
			test_scene_b = obs_scene_create("__mfd_selftest_scene_b__");

			obs_data_t *red_settings = obs_data_create();
			obs_data_set_int(red_settings, "color", 0xFF0000FFLL);
			obs_data_set_int(red_settings, "width", 1920);
			obs_data_set_int(red_settings, "height", 1080);
			src_a = obs_source_create("color_source", "__mfd_selftest_source_a__", red_settings, nullptr);
			obs_data_release(red_settings);

			obs_data_t *blue_settings = obs_data_create();
			obs_data_set_int(blue_settings, "color", 0xFFFF0000LL);
			obs_data_set_int(blue_settings, "width", 1920);
			obs_data_set_int(blue_settings, "height", 1080);
			src_b = obs_source_create("color_source", "__mfd_selftest_source_b__", blue_settings, nullptr);
			obs_data_release(blue_settings);

			if (src_a && obs_source_get_name(src_a)) {
				std::snprintf(src_a_actual_name, sizeof(src_a_actual_name), "%s",
					      obs_source_get_name(src_a));
			}
			if (src_b && obs_source_get_name(src_b)) {
				std::snprintf(src_b_actual_name, sizeof(src_b_actual_name), "%s",
					      obs_source_get_name(src_b));
			}

			obs_scene_add(test_scene, src_a);
			obs_scene_add(test_scene, src_b);

			obs_frontend_set_current_scene(obs_scene_get_source(test_scene));
		},
		Qt::BlockingQueuedConnection);

	/* Wait for program scene transition to settle */
	std::this_thread::sleep_for(std::chrono::milliseconds(400));

	obs_data_t *req = obs_data_create();
	obs_data_set_string(req, "name", "apc40mk2");
	obs_data_t *res = call_real_ws_vendor_request("load_profile", req);
	if (res) {
		obs_data_release(res);
	}
	obs_data_release(req);

	req = obs_data_create();
	obs_data_set_string(req, "slot", "fader_1");
	obs_data_set_string(req, "target_mode", "crossfade_ab");
	obs_data_set_string(req, "curve", "dip_free");
	obs_data_set_string(req, "source_a", src_a_actual_name);
	obs_data_set_string(req, "source_b", src_b_actual_name);
	res = call_real_ws_vendor_request("set_slot", req);
	bool slot_avail = res ? obs_data_get_bool(res, "available") : false;
	if (res) {
		obs_data_release(res);
	}
	obs_data_release(req);

	/* Measure isolated source brightness at t=0.0 (pure Red) and t=1.0 (pure Blue) on program output */
	char program_scene_name[128] = "__mfd_selftest_scene__";
	QMetaObject::invokeMethod(
		main_win,
		[&]() {
			obs_source_t *cur = obs_frontend_get_current_scene();
			if (cur) {
				const char *nm = obs_source_get_name(cur);
				if (nm) {
					std::snprintf(program_scene_name, sizeof(program_scene_name), "%s", nm);
				}
				obs_source_release(cur);
			}
		},
		Qt::BlockingQueuedConnection);

	auto set_fader1_and_wait = [&](double t_val) {
		obs_data_t *r = obs_data_create();
		obs_data_set_string(r, "slot", "fader_1");
		obs_data_set_double(r, "value", t_val);
		obs_data_t *rs = call_real_ws_vendor_request("set_fader_value", r);
		if (rs) {
			obs_data_release(rs);
		}
		obs_data_release(r);
		std::this_thread::sleep_for(std::chrono::milliseconds(80));
	};

	set_fader1_and_wait(0.0);
	float r0 = 0.0f, g0 = 0.0f, b0 = 0.0f, srgb0 = 0.0f;
	bool ss0_ok = capture_source_screenshot_avg_rgb(program_scene_name, &r0, &g0, &b0, &srgb0);
	float bright_red = r0 + g0 + b0;

	set_fader1_and_wait(1.0);
	float r1 = 0.0f, g1 = 0.0f, b1 = 0.0f, srgb1 = 0.0f;
	bool ss1_ok = capture_source_screenshot_avg_rgb(program_scene_name, &r1, &g1, &b1, &srgb1);
	float bright_blue = r1 + g1 + b1;
	float min_source_brightness = std::min(bright_red, bright_blue);

	obs_source_t *flt_a = src_a ? obs_source_get_filter_by_name(src_a, MFD_INTERNAL_OPACITY_FILTER_NAME) : nullptr;
	obs_source_t *flt_b = src_b ? obs_source_get_filter_by_name(src_b, MFD_INTERNAL_OPACITY_FILTER_NAME) : nullptr;
	float read_a_end = flt_a ? mfd_layer_filter_get_opacity(flt_a) : -1.0f;
	float read_b_end = flt_b ? mfd_layer_filter_get_opacity(flt_b) : -1.0f;

	/* Check (2): Program output GetSourceScreenshot at t = 0.25, 0.5, 0.75 */
	const float test_ts[3] = {0.25f, 0.50f, 0.75f};
	bool screenshot_all_pass = ss0_ok && ss1_ok;
	float read_a_mid = -1.0f;
	float read_b_mid = -1.0f;

	for (int i = 0; i < 3; ++i) {
		const float t = test_ts[i];
		set_fader1_and_wait(t);
		if (i == 1) {
			read_a_mid = flt_a ? mfd_layer_filter_get_opacity(flt_a) : -1.0f;
			read_b_mid = flt_b ? mfd_layer_filter_get_opacity(flt_b) : -1.0f;
		}
		float avg_r = 0.0f, avg_g = 0.0f, avg_b = 0.0f, srgb_bright = 0.0f;
		bool cap_ok =
			capture_source_screenshot_avg_rgb(program_scene_name, &avg_r, &avg_g, &avg_b, &srgb_bright);
		const float exp_r = 1.0f - t;
		const float exp_g = 0.0f;
		const float exp_b = t;
		const float err_r = std::fabs(avg_r - exp_r);
		const float err_g = std::fabs(avg_g - exp_g);
		const float err_b = std::fabs(avg_b - exp_b);
		const float brightness = avg_r + avg_g + avg_b;
		const bool mix_ok = cap_ok && (err_r <= 0.02f) && (err_g <= 0.02f) && (err_b <= 0.02f);
		/* 8-bit sRGB integer quantization of 224/255 is 0.0044 (1 LSB = 0.0075); require no dip in both linear and sRGB */
		const bool bright_ok = cap_ok && (brightness >= min_source_brightness - 0.008f) &&
				       (srgb_bright >= srgb0 - 1e-4f) && (srgb_bright >= srgb1 - 1e-4f);
		const bool step_ok = mix_ok && bright_ok;
		if (!step_ok) {
			screenshot_all_pass = false;
		}
		obs_log(step_ok ? LOG_INFO : LOG_ERROR,
			"SCREENSHOT CHECK t=%.2f [%s]: program='%s' avg_rgb=(%.4f, %.4f, %.4f) expected=(%.4f, %.4f, %.4f) max_err=%.4f brightness=%.4f srgb_brightness=%.4f (src_red=%.4f src_blue=%.4f) mix_within_2pct=%d bright_no_dip=%d",
			t, step_ok ? "PASS" : "FAIL", program_scene_name, avg_r, avg_g, avg_b, exp_r, exp_g, exp_b,
			std::max({err_r, err_g, err_b}), brightness, srgb_bright, bright_red, bright_blue, (int)mix_ok,
			(int)bright_ok);
	}

	if (flt_a) {
		obs_source_release(flt_a);
	}
	if (flt_b) {
		obs_source_release(flt_b);
	}

	/* Switch to Studio Mode for T-bar & loopMIDI checks */
	QMetaObject::invokeMethod(
		main_win,
		[&]() {
			if (!obs_frontend_preview_program_mode_active()) {
				obs_frontend_set_preview_program_mode(true);
			}
			if (test_scene_b) {
				obs_frontend_set_current_preview_scene(obs_scene_get_source(test_scene_b));
			}
			refresh_studio_mode_state_on_ui_thread();
		},
		Qt::BlockingQueuedConnection);

	req = obs_data_create();
	obs_data_set_string(req, "slot", "crossfader");
	obs_data_set_double(req, "value", 0.5);
	res = call_real_ws_vendor_request("set_fader_value", req);
	bool mid_ok = res && (obs_data_get_double(res, "value") > 0.49);
	int slider_mid = -1;
	QMetaObject::invokeMethod(
		main_win, [&]() { slider_mid = g_tbar_slider ? g_tbar_slider->value() : -1; },
		Qt::BlockingQueuedConnection);
	if (res) {
		obs_data_release(res);
	}
	obs_data_release(req);

	req = obs_data_create();
	obs_data_set_string(req, "slot", "crossfader");
	obs_data_set_double(req, "value", 1.0);
	obs_data_set_bool(req, "release", true);
	res = call_real_ws_vendor_request("set_fader_value", req);
	bool end_ok = res && (obs_data_get_double(res, "value") == 0.0);
	bool completed = (eng->fader_slots[0].takeover.state == MFD_TAKEOVER_PARKED);
	if (res) {
		obs_data_release(res);
	}
	obs_data_release(req);

	/* Allow the vendor API T-bar release transition to finish before loopMIDI sweeps */
	std::this_thread::sleep_for(std::chrono::milliseconds(550));

	/* Check (1): loopMIDI + tests/midi-send.c live CC sweep & soft takeover test */
	char loopmidi_port[128] = {0};
	struct mfd_midi_device_info devs[32];
	size_t dev_count = mfd_winmm_enumerate(devs, 32);
	for (size_t i = 0; i < dev_count; ++i) {
		if (strstr(devs[i].name, "loopMIDI") != nullptr || strstr(devs[i].name, "loopmidi") != nullptr) {
			std::snprintf(loopmidi_port, sizeof(loopmidi_port), "%s", devs[i].name);
			break;
		}
	}

	bool loopmidi_tested = false;
	bool loopmidi_pass = false;
	if (loopmidi_port[0] != '\0') {
		loopmidi_tested = true;
		char prev_midi_dev[MFD_MIDI_DEV_NAME_MAX] = {0};
		std::snprintf(prev_midi_dev, sizeof(prev_midi_dev), "%s",
			      eng->midi_dev.opened_name[0] != '\0' ? eng->midi_dev.opened_name
								   : eng->configured_device_name);
		QString midi_send_exe = qEnvironmentVariable(
			"MFD_MIDI_SEND_EXE", "C:/midi-fadester/build_x64/tests/RelWithDebInfo/midi-send.exe");
		bool connected = mfd_engine_reconnect_midi(eng, loopmidi_port);

		/* Configure crossfader on Ch 1 CC 15 -> tbar, synced at 0.50 so a sweep from 0 starts away from current value */
		req = obs_data_create();
		obs_data_set_string(req, "name", "apc40mk2");
		res = call_real_ws_vendor_request("load_profile", req);
		if (res) {
			obs_data_release(res);
		}
		obs_data_release(req);

		/* Place crossfader at 0.50 (512) and arm soft takeover waiting for physical fader to cross 0.50 */
		mfd_takeover_sync_external(&eng->fader_slots[0].takeover, 0.50f);
		mfd_smoother_reset(&eng->fader_slots[0].smoother, 0.50f);
		eng->fader_slots[0].last_applied_val = 0.50f;
		g_last_pos_1024.store(512, std::memory_order_relaxed);
		QMetaObject::invokeMethod(
			main_win, [&]() { execute_tbar_on_ui_thread(512, true, false); }, Qt::BlockingQueuedConnection);

		/* Part 1a: Sweep 0..30 (0.00..0.236, away from 0.50) -> soft takeover must stay WAITING and T-bar must stay at 512 */
		int rc_pre = QProcess::execute(midi_send_exe, QStringList() << QString::fromUtf8(loopmidi_port) << "1"
									    << "15" << "60"
									    << "0" << "30");
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
		bool pre_waiting_ok = (rc_pre == 0) && (eng->fader_slots[0].takeover.state == MFD_TAKEOVER_WAITING) &&
				      (eng->last_midi_decoded.raw_value == 30);
		int slider_pre = -1;
		QMetaObject::invokeMethod(
			main_win, [&]() { slider_pre = g_tbar_slider ? g_tbar_slider->value() : -1; },
			Qt::BlockingQueuedConnection);
		bool pre_slider_held = (slider_pre == 512);

		/* Part 1b: Full sweep 0..127 over 450 ms -> soft takeover engages at ~64 (0.50), T-bar follows within smoothing window, completes at 90% and parks */
		mfd_takeover_sync_external(&eng->fader_slots[0].takeover, 0.50f);
		mfd_smoother_reset(&eng->fader_slots[0].smoother, 0.50f);
		eng->fader_slots[0].last_applied_val = 0.50f;
		g_last_pos_1024.store(512, std::memory_order_relaxed);
		QMetaObject::invokeMethod(
			main_win, [&]() { execute_tbar_on_ui_thread(512, true, false); }, Qt::BlockingQueuedConnection);

		QProcess sweep_proc;
		sweep_proc.start(midi_send_exe, QStringList() << QString::fromUtf8(loopmidi_port) << "1" << "15"
							      << "450" << "0"
							      << "127");
		sweep_proc.waitForStarted(1000);

		bool saw_waiting_before_pickup = false;
		bool saw_engaged_after_pickup = false;
		int mid_sweep_samples = 0;
		float max_smoothing_lag = 0.0f;

		for (int poll = 0; poll < 35; ++poll) {
			std::this_thread::sleep_for(std::chrono::milliseconds(16));
			uint16_t cc_now = eng->last_midi_decoded.raw_value;
			enum mfd_takeover_state st_now = eng->fader_slots[0].takeover.state;
			float target_now = eng->fader_slots[0].smoother.target;
			int slider_now = -1;
			QMetaObject::invokeMethod(
				main_win, [&]() { slider_now = g_tbar_slider ? g_tbar_slider->value() : -1; },
				Qt::BlockingQueuedConnection);

			if (cc_now > 5 && cc_now < 58 && st_now == MFD_TAKEOVER_WAITING && slider_now == 512) {
				saw_waiting_before_pickup = true;
			}
			if (cc_now >= 66 && cc_now <= 110 && st_now == MFD_TAKEOVER_ENGAGED && slider_now > 512) {
				saw_engaged_after_pickup = true;
				float tbar_norm = static_cast<float>(slider_now) / 1023.0f;
				float lag = std::fabs(target_now - tbar_norm);
				if (lag > max_smoothing_lag) {
					max_smoothing_lag = lag;
				}
				++mid_sweep_samples;
			}
			if (sweep_proc.state() == QProcess::NotRunning && poll > 28) {
				break;
			}
		}
		sweep_proc.waitForFinished(2000);
		std::this_thread::sleep_for(std::chrono::milliseconds(550));

		bool sweep_completed_throw = (eng->fader_slots[0].takeover.state == MFD_TAKEOVER_PARKED);
		uint16_t final_cc = eng->last_midi_decoded.raw_value;
		bool smoothing_ok = (mid_sweep_samples >= 2) && (max_smoothing_lag <= 0.12f);

		/*
		 * Case (2): loopMIDI bidirectional sweep 0 -> 127, dwell, then 127 -> 0.
		 * Assert:
		 * - exactly 1 "started" and 1 "finished" per direction,
		 * - T-bar released once per throw,
		 * - 0 slider writes between completion and the fader leaving the rail (> 8% from peak).
		 */
		mfd_takeover_init(&eng->fader_slots[0].takeover, 0.0f, MFD_DEFAULT_PICKUP_THRESHOLD, true);
		eng->fader_slots[0].takeover.state = MFD_TAKEOVER_ENGAGED;
		mfd_smoother_reset(&eng->fader_slots[0].smoother, 0.0f);
		eng->fader_slots[0].last_applied_val = 0.0f;
		g_last_pos_1024.store(0, std::memory_order_relaxed);
		g_obs_manual_started.store(0, std::memory_order_relaxed);
		g_obs_manual_finished.store(0, std::memory_order_relaxed);
		g_obs_manual_cancelled.store(0, std::memory_order_relaxed);
		g_tbar_release_count.store(0, std::memory_order_relaxed);
		g_tbar_writes_since_release.store(0, std::memory_order_relaxed);
		g_tbar_writes_before_unpark.store(0, std::memory_order_relaxed);
		g_tbar_awaiting_unpark.store(false, std::memory_order_relaxed);

		int rc_c2_d1 = QProcess::execute(midi_send_exe, QStringList() << QString::fromUtf8(loopmidi_port) << "1"
									      << "15" << "400"
									      << "0" << "127");
		/* Dwell at rail (550 ms) */
		std::this_thread::sleep_for(std::chrono::milliseconds(550));

		int c2_d1_started = g_obs_manual_started.load(std::memory_order_relaxed);
		int c2_d1_finished = g_obs_manual_finished.load(std::memory_order_relaxed);
		int c2_d1_releases = g_tbar_release_count.load(std::memory_order_relaxed);
		int c2_d1_post_writes = g_tbar_writes_since_release.load(std::memory_order_relaxed);
		bool c2_d1_parked = (eng->fader_slots[0].takeover.state == MFD_TAKEOVER_PARKED);
		float c2_d1_peak = eng->fader_slots[0].takeover.parked_peak;

		g_obs_manual_started.store(0, std::memory_order_relaxed);
		g_obs_manual_finished.store(0, std::memory_order_relaxed);
		g_tbar_release_count.store(0, std::memory_order_relaxed);

		int rc_c2_d2 = QProcess::execute(midi_send_exe, QStringList() << QString::fromUtf8(loopmidi_port) << "1"
									      << "15" << "400"
									      << "127" << "0");
		std::this_thread::sleep_for(std::chrono::milliseconds(550));

		int c2_d2_started = g_obs_manual_started.load(std::memory_order_relaxed);
		int c2_d2_finished = g_obs_manual_finished.load(std::memory_order_relaxed);
		int c2_d2_releases = g_tbar_release_count.load(std::memory_order_relaxed);
		int c2_d2_post_writes = g_tbar_writes_since_release.load(std::memory_order_relaxed);
		int c2_writes_before_unpark = g_tbar_writes_before_unpark.load(std::memory_order_relaxed);
		bool c2_d2_parked = (eng->fader_slots[0].takeover.state == MFD_TAKEOVER_PARKED);
		float c2_d2_peak = eng->fader_slots[0].takeover.parked_peak;

		bool case2_ok = (rc_c2_d1 == 0) && (rc_c2_d2 == 0) && (c2_d1_started == 1) && (c2_d1_finished == 1) &&
				(c2_d1_releases == 1) && (c2_d1_post_writes == 0) && c2_d1_parked &&
				(c2_d2_started == 1) && (c2_d2_finished == 1) && (c2_d2_releases == 1) &&
				(c2_d2_post_writes == 0) && (c2_writes_before_unpark == 0) && c2_d2_parked;

		obs_log(case2_ok ? LOG_INFO : LOG_ERROR,
			"LOOPMIDI CASE 2 (0->127->0) [%s]: dir1(started=%d finished=%d releases=%d parked=%d peak=%.4f post_completion_writes=%d) dir2(started=%d finished=%d releases=%d parked=%d peak=%.4f post_completion_writes=%d writes_before_leaving_rail=%d)",
			case2_ok ? "PASS" : "FAIL", c2_d1_started, c2_d1_finished, c2_d1_releases, (int)c2_d1_parked,
			c2_d1_peak, c2_d1_post_writes, c2_d2_started, c2_d2_finished, c2_d2_releases, (int)c2_d2_parked,
			c2_d2_peak, c2_d2_post_writes, c2_writes_before_unpark);

		/*
		 * Case (3): loopMIDI sweep that stops at 95% (CC 121 = 95.28%), dwells, then comes back (121 -> 0).
		 * Assert:
		 * - exactly 1 "started" and 1 "finished" per direction,
		 * - T-bar released once per throw,
		 * - 0 slider writes between completion and the fader leaving the 95% peak (> 8% below 95%).
		 */
		mfd_takeover_init(&eng->fader_slots[0].takeover, 0.0f, MFD_DEFAULT_PICKUP_THRESHOLD, true);
		eng->fader_slots[0].takeover.state = MFD_TAKEOVER_ENGAGED;
		mfd_smoother_reset(&eng->fader_slots[0].smoother, 0.0f);
		eng->fader_slots[0].last_applied_val = 0.0f;
		g_last_pos_1024.store(0, std::memory_order_relaxed);
		g_obs_manual_started.store(0, std::memory_order_relaxed);
		g_obs_manual_finished.store(0, std::memory_order_relaxed);
		g_tbar_release_count.store(0, std::memory_order_relaxed);
		g_tbar_writes_since_release.store(0, std::memory_order_relaxed);
		g_tbar_writes_before_unpark.store(0, std::memory_order_relaxed);
		g_tbar_awaiting_unpark.store(false, std::memory_order_relaxed);

		int rc_c3_d1 = QProcess::execute(midi_send_exe, QStringList() << QString::fromUtf8(loopmidi_port) << "1"
									      << "15" << "400"
									      << "0" << "121");
		/* Dwell at 95% (550 ms) */
		std::this_thread::sleep_for(std::chrono::milliseconds(550));

		int c3_d1_started = g_obs_manual_started.load(std::memory_order_relaxed);
		int c3_d1_finished = g_obs_manual_finished.load(std::memory_order_relaxed);
		int c3_d1_releases = g_tbar_release_count.load(std::memory_order_relaxed);
		int c3_d1_post_writes = g_tbar_writes_since_release.load(std::memory_order_relaxed);
		bool c3_d1_parked = (eng->fader_slots[0].takeover.state == MFD_TAKEOVER_PARKED);
		float c3_d1_peak = eng->fader_slots[0].takeover.parked_peak;

		g_obs_manual_started.store(0, std::memory_order_relaxed);
		g_obs_manual_finished.store(0, std::memory_order_relaxed);
		g_tbar_release_count.store(0, std::memory_order_relaxed);

		int rc_c3_d2 = QProcess::execute(midi_send_exe, QStringList() << QString::fromUtf8(loopmidi_port) << "1"
									      << "15" << "400"
									      << "121" << "0");
		std::this_thread::sleep_for(std::chrono::milliseconds(550));

		int c3_d2_started = g_obs_manual_started.load(std::memory_order_relaxed);
		int c3_d2_finished = g_obs_manual_finished.load(std::memory_order_relaxed);
		int c3_d2_releases = g_tbar_release_count.load(std::memory_order_relaxed);
		int c3_d2_post_writes = g_tbar_writes_since_release.load(std::memory_order_relaxed);
		int c3_writes_before_unpark = g_tbar_writes_before_unpark.load(std::memory_order_relaxed);
		bool c3_d2_parked = (eng->fader_slots[0].takeover.state == MFD_TAKEOVER_PARKED);
		float c3_d2_peak = eng->fader_slots[0].takeover.parked_peak;

		bool case3_ok = (rc_c3_d1 == 0) && (rc_c3_d2 == 0) && (c3_d1_started == 1) && (c3_d1_finished == 1) &&
				(c3_d1_releases == 1) && (c3_d1_post_writes == 0) && c3_d1_parked &&
				(std::fabs(c3_d1_peak - (121.0f / 127.0f)) < 1e-4f) && (c3_d2_started == 1) &&
				(c3_d2_finished == 1) && (c3_d2_releases == 1) && (c3_d2_post_writes == 0) &&
				(c3_writes_before_unpark == 0) && c3_d2_parked;

		obs_log(case3_ok ? LOG_INFO : LOG_ERROR,
			"LOOPMIDI CASE 3 (0->95%%->0) [%s]: dir1(started=%d finished=%d releases=%d parked=%d peak=%.4f post_completion_writes=%d) dir2(started=%d finished=%d releases=%d parked=%d peak=%.4f post_completion_writes=%d writes_before_leaving_rail=%d)",
			case3_ok ? "PASS" : "FAIL", c3_d1_started, c3_d1_finished, c3_d1_releases, (int)c3_d1_parked,
			c3_d1_peak, c3_d1_post_writes, c3_d2_started, c3_d2_finished, c3_d2_releases, (int)c3_d2_parked,
			c3_d2_peak, c3_d2_post_writes, c3_writes_before_unpark);

		loopmidi_pass = connected && pre_waiting_ok && pre_slider_held && saw_waiting_before_pickup &&
				saw_engaged_after_pickup && smoothing_ok && sweep_completed_throw &&
				(final_cc == 127) && case2_ok && case3_ok;

		obs_log(loopmidi_pass ? LOG_INFO : LOG_ERROR,
			"LOOPMIDI SWEEP CHECK [%s]: port='%s' connected=%d pre_waiting=%d(slider=%d) saw_waiting_pre_pickup=%d saw_engaged_post_pickup=%d mid_samples=%d max_smoothing_lag=%.4f final_cc=%u throw_completed=%d case2_ok=%d case3_ok=%d",
			loopmidi_pass ? "PASS" : "FAIL", loopmidi_port, (int)connected, (int)pre_waiting_ok, slider_pre,
			(int)saw_waiting_before_pickup, (int)saw_engaged_after_pickup, mid_sweep_samples,
			max_smoothing_lag, (unsigned)final_cc, (int)sweep_completed_throw, (int)case2_ok,
			(int)case3_ok);

		mfd_engine_reconnect_midi(eng, prev_midi_dev);
	} else {
		obs_log(LOG_INFO, "LOOPMIDI SWEEP CHECK [SKIPPED]: no WinMM input port matching 'loopMIDI' found.");
	}

	/* In-Process Bridge (proc + signal) and Visual Sidechain cross-plugin arrival */
	selftest_sig_capture sig_cap{};
	signal_handler_t *sh = obs_get_signal_handler();
	proc_handler_t *ph = obs_get_proc_handler();
	if (sh) {
		signal_handler_connect(sh, "midi_fadester_fader_changed", on_selftest_fader_signal, &sig_cap);
	}

	bool bridge_set_ok = false;
	bool bridge_get_ok = false;
	float bridge_get_val = -1.0f;
	float vs_fader2_val = -1.0f;

	if (ph) {
		struct calldata cd;
		calldata_init(&cd);
		calldata_set_string(&cd, "slot", "fader_2");
		calldata_set_float(&cd, "value", 0.65);
		if (proc_handler_call(ph, "midi_fadester_set_fader_value", &cd)) {
			bridge_set_ok = calldata_bool(&cd, "ok");
		}
		calldata_free(&cd);

		calldata_init(&cd);
		calldata_set_string(&cd, "slot", "fader_2");
		if (proc_handler_call(ph, "midi_fadester_get_slot_value", &cd)) {
			bridge_get_ok = calldata_bool(&cd, "ok");
			bridge_get_val = static_cast<float>(calldata_float(&cd, "value"));
		}
		calldata_free(&cd);
	}

	if (sh) {
		signal_handler_disconnect(sh, "midi_fadester_fader_changed", on_selftest_fader_signal, &sig_cap);
	}

	/* Verify Visual Sidechain (commit 26d9089) receives fader_2=0.65 via in-process bridge and modulates sibling filter */
	obs_source_t *vs_test_src = obs_source_create("color_source", "__mfd_vs_bridge_src__", nullptr, nullptr);
	obs_source_t *vs_col_flt = obs_source_create("color_filter", "__mfd_vs_col__", nullptr, nullptr);
	obs_data_t *vs_mod_set = obs_data_create();
	obs_data_set_bool(vs_mod_set, "slot_1_enabled", true);
	obs_data_set_string(vs_mod_set, "slot_1_target_filter", "__mfd_vs_col__");
	obs_data_set_string(vs_mod_set, "slot_1_target_setting", "opacity");
	obs_data_set_int(vs_mod_set, "slot_1_signal", 202); /* VS_SIG_FADER_2 */
	obs_data_set_double(vs_mod_set, "slot_1_amount_pct", 100.0);
	obs_data_set_double(vs_mod_set, "slot_1_min_value", 0.0);
	obs_data_set_double(vs_mod_set, "slot_1_max_value", 1.0);
	obs_source_t *vs_mod_flt =
		obs_source_create("visual_sidechain_modulator_filter", "__mfd_vs_mod__", vs_mod_set, nullptr);
	obs_data_release(vs_mod_set);

	if (vs_test_src && vs_col_flt && vs_mod_flt) {
		obs_source_filter_add(vs_test_src, vs_col_flt);
		obs_source_filter_add(vs_test_src, vs_mod_flt);
		if (ph) {
			struct calldata cd;
			calldata_init(&cd);
			calldata_set_string(&cd, "slot", "fader_2");
			calldata_set_float(&cd, "value", 0.65);
			proc_handler_call(ph, "midi_fadester_set_fader_value", &cd);
			calldata_free(&cd);
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(60));
		obs_data_t *col_s = obs_source_get_settings(vs_col_flt);
		if (col_s) {
			vs_fader2_val = static_cast<float>(obs_data_get_double(col_s, "opacity"));
			obs_data_release(col_s);
		}
	}
	if (vs_mod_flt) {
		if (vs_test_src) {
			obs_source_filter_remove(vs_test_src, vs_mod_flt);
		}
		obs_source_release(vs_mod_flt);
	}
	if (vs_col_flt) {
		if (vs_test_src) {
			obs_source_filter_remove(vs_test_src, vs_col_flt);
		}
		obs_source_release(vs_col_flt);
	}
	if (vs_test_src) {
		obs_source_remove(vs_test_src);
		obs_source_release(vs_test_src);
	}

	req = obs_data_create();
	obs_data_set_string(req, "name", "apc40mk2");
	res = call_real_ws_vendor_request("load_profile", req);
	if (res) {
		obs_data_release(res);
	}
	obs_data_release(req);

	/* Clean up test scene and restore previous state on UI thread */
	QMetaObject::invokeMethod(
		main_win,
		[&]() {
			if (prev_scene) {
				obs_frontend_set_current_scene(prev_scene);
				obs_source_release(prev_scene);
			}
			if (src_a) {
				obs_source_remove(src_a);
				obs_source_release(src_a);
			}
			if (src_b) {
				obs_source_remove(src_b);
				obs_source_release(src_b);
			}
			if (test_scene_b) {
				obs_source_remove(obs_scene_get_source(test_scene_b));
				obs_scene_release(test_scene_b);
			}
			if (test_scene) {
				obs_source_remove(obs_scene_get_source(test_scene));
				obs_scene_release(test_scene);
			}
			if (!was_studio) {
				obs_frontend_set_preview_program_mode(false);
				refresh_studio_mode_state_on_ui_thread();
			}
		},
		Qt::BlockingQueuedConnection);

	base_set_log_handler(g_orig_log_handler, g_orig_log_param);

	obs_log(LOG_INFO,
		"SELF-TEST RESULT: slot_avail=%d filter_a=%.2f->%.2f filter_b=%.2f->%.2f screenshot_all_pass=%d loopmidi_tested=%d loopmidi_pass=%d slider_mid=%d tbar_mid_ok=%d tbar_end_ok=%d tbar_completed=%d bridge_ok=%d/%.2f sig_val=%.2f(%s) vs_received_fader_2=%.2f",
		(int)slot_avail, read_a_mid, read_a_end, read_b_mid, read_b_end, (int)screenshot_all_pass,
		(int)loopmidi_tested, (int)loopmidi_pass, slider_mid, (int)mid_ok, (int)end_ok, (int)completed,
		(int)(bridge_set_ok && bridge_get_ok), bridge_get_val, sig_cap.value, sig_cap.slot, vs_fader2_val);
}

static void run_live_selftest_if_requested(void)
{
	if (!qEnvironmentVariableIsSet("MFD_SELF_TEST")) {
		return;
	}
	std::thread(run_live_selftest_worker).detach();
}
#endif

static void on_tbar_frontend_event(enum obs_frontend_event event, void *unused)
{
	UNUSED_PARAMETER(unused);
	switch (event) {
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
		refresh_studio_mode_state_on_ui_thread();
#ifdef MFD_ENABLE_SELF_TEST
		run_live_selftest_if_requested();
#endif
		break;
	case OBS_FRONTEND_EVENT_STUDIO_MODE_ENABLED:
	case OBS_FRONTEND_EVENT_STUDIO_MODE_DISABLED:
	case OBS_FRONTEND_EVENT_TRANSITION_STOPPED:
		refresh_studio_mode_state_on_ui_thread();
		break;
	default:
		break;
	}
}

bool mfd_tbar_is_studio_mode_active(void)
{
	QWidget *main_win = static_cast<QWidget *>(obs_frontend_get_main_window());
	if (main_win && QThread::currentThread() == main_win->thread()) {
		bool active = obs_frontend_preview_program_mode_active();
		g_studio_mode_active.store(active, std::memory_order_relaxed);
		if (active && g_tbar_slider.isNull()) {
			refresh_studio_mode_state_on_ui_thread();
		}
	}
	return g_studio_mode_active.load(std::memory_order_relaxed);
}

bool mfd_tbar_driver_apply(float t_norm, bool *out_completed_throw, char *out_err, size_t err_size)
{
	if (out_completed_throw) {
		*out_completed_throw = false;
	}
	if (out_err && err_size > 0U) {
		out_err[0] = '\0';
	}

	bool studio_active = mfd_tbar_is_studio_mode_active();
	bool slider_found = g_slider_tbar_found.load(std::memory_order_relaxed);
	int last_pos = g_last_pos_1024.load(std::memory_order_relaxed);

	struct mfd_tbar_action act;
	mfd_tbar_compute_action(studio_active, slider_found, t_norm, last_pos, &act);

	if (act.report_missing_slider_error) {
		if (!g_logged_missing_error.exchange(true, std::memory_order_relaxed)) {
			obs_log(LOG_ERROR,
				"CRITICAL: Studio Mode is active, but Qt QSlider 'slider-tbar' was not found in OBS main window! T-bar sync disabled.");
		}
		if (out_err && err_size > 0U) {
			std::snprintf(
				out_err, err_size,
				"CRITICAL: Studio Mode 'slider-tbar' widget not found in this OBS version. T-bar sync disabled.");
		}
		return false;
	}

	if (!act.allow_frontend_calls) {
		return true;
	}

	if (act.call_release_tbar) {
		g_last_pos_1024.store(0, std::memory_order_relaxed);
		if (out_completed_throw) {
			*out_completed_throw = true;
		}
	} else {
		g_last_pos_1024.store(act.target_pos_1024, std::memory_order_relaxed);
	}

	if (!act.call_set_tbar && !act.call_release_tbar) {
		return true;
	}

	if (g_tbar_awaiting_unpark.load(std::memory_order_relaxed)) {
		struct mfd_engine *eng_now = mfd_engine_get();
		if (eng_now) {
			const struct mfd_takeover &tk = eng_now->fader_slots[0].takeover;
			const float retreat = std::fabs(tk.parked_peak - eng_now->fader_slots[0].last_raw_norm);
			if (tk.state == MFD_TAKEOVER_PARKED || retreat <= MFD_PARKED_UNPARK_HYSTERESIS) {
				g_tbar_writes_before_unpark.fetch_add(1, std::memory_order_relaxed);
			} else {
				g_tbar_awaiting_unpark.store(false, std::memory_order_relaxed);
			}
		}
	}
	if (act.call_release_tbar) {
		g_tbar_awaiting_unpark.store(true, std::memory_order_relaxed);
	}

	QWidget *main_win = static_cast<QWidget *>(obs_frontend_get_main_window());
	if (!main_win) {
		return true;
	}

	const int pos = act.target_pos_1024;
	const bool do_set = act.call_set_tbar;
	const bool do_rel = act.call_release_tbar;

	if (QThread::currentThread() == main_win->thread()) {
		execute_tbar_on_ui_thread(pos, do_set, do_rel);
	} else {
		QMetaObject::invokeMethod(
			main_win, [pos, do_set, do_rel]() { execute_tbar_on_ui_thread(pos, do_set, do_rel); },
			Qt::QueuedConnection);
	}

	return true;
}

void mfd_tbar_init(void)
{
	g_studio_mode_active.store(false, std::memory_order_relaxed);
	g_slider_tbar_found.store(false, std::memory_order_relaxed);
	g_logged_missing_error.store(false, std::memory_order_relaxed);
	g_last_pos_1024.store(0, std::memory_order_relaxed);

	mfd_engine_set_tbar_driver(mfd_tbar_driver_apply);
	obs_frontend_add_event_callback(on_tbar_frontend_event, nullptr);
}

void mfd_tbar_shutdown(void)
{
	obs_frontend_remove_event_callback(on_tbar_frontend_event, nullptr);
	mfd_engine_set_tbar_driver(nullptr);
	g_tbar_slider.clear();
}
