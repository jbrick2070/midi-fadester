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
#include <QCoreApplication>
#include <QEventLoop>
#include <QFileInfo>
#include <QImage>
#include <QMetaObject>
#include <QPointer>
#include <QProcess>
#include <QPushButton>
#include <QSlider>
#include <QThread>
#include <QVariant>
#include <QWidget>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>

static QPointer<QWidget> g_main_win;
static QPointer<QSlider> g_tbar_slider;
static std::atomic<bool> g_studio_mode_active{false};
static std::atomic<bool> g_slider_tbar_found{false};
static std::atomic<bool> g_logged_missing_error{false};
static std::atomic<int> g_last_pos_1024{0};
static std::atomic<bool> g_tbar_release_latched{false};
static std::atomic<uint64_t> g_tbar_throw_epoch{1ULL};
static std::atomic<int> g_tbar_set_count{0};
static std::atomic<int> g_tbar_release_count{0};
static std::atomic<int> g_tbar_first_release_raw_cc{-1};
static std::atomic<int> g_tbar_releases_before_cc124{0};
static std::atomic<int> g_tbar_writes_since_release{0};
static std::atomic<int> g_tbar_post_first_release_writes{0};
static std::atomic<int> g_tbar_writes_before_unpark{0};
static std::atomic<bool> g_tbar_awaiting_unpark{false};
static std::atomic<bool> g_tbar_debug_log{false};

/* UI-thread-only per-transition latch & active-transition guard state (zero timers) */
static uint64_t g_tbar_ui_last_released_epoch = 0ULL;
static bool g_tbar_ui_throw_active = false;
static bool g_tbar_awaiting_transition_stop = false;

struct mfd_deferred_tbar_cmd {
	bool active;
	int pos_1024;
	bool do_set;
	bool do_release;
	uint64_t epoch;
};

static mfd_deferred_tbar_cmd g_pending_deferred_tbar = {false, 0, false, false, 0ULL};

void mfd_tbar_reset_release_latch(void)
{
	g_tbar_throw_epoch.fetch_add(1ULL, std::memory_order_acq_rel);
	g_tbar_release_latched.store(false, std::memory_order_release);
	g_last_pos_1024.store(0, std::memory_order_relaxed);
}

bool mfd_tbar_is_release_latched(void)
{
	return g_tbar_release_latched.load(std::memory_order_acquire);
}

static const char *takeover_state_str(enum mfd_takeover_state st)
{
	switch (st) {
	case MFD_TAKEOVER_ENGAGED:
		return "ENGAGED";
	case MFD_TAKEOVER_WAITING:
		return "WAITING";
	case MFD_TAKEOVER_PARKED:
		return "PARKED";
	default:
		return "UNKNOWN";
	}
}

static QSlider *locate_slider_tbar_on_ui_thread(void)
{
	if (!g_main_win) {
		g_main_win = static_cast<QWidget *>(obs_frontend_get_main_window());
	}
	QWidget *main_win = g_main_win.data();
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

#ifdef MFD_ENABLE_SELF_TEST
static QPushButton *locate_studio_transition_button_on_ui_thread(void)
{
	auto match_in_widget = [](QWidget *root) -> QPushButton * {
		if (!root) {
			return nullptr;
		}
		const QList<QPushButton *> btns = root->findChildren<QPushButton *>();
		for (QPushButton *b : btns) {
			if (!b || !b->isVisible()) {
				continue;
			}
			QString txt = b->text();
			txt.remove(QLatin1Char('&'));
			txt = txt.trimmed();
			if (txt.compare(QStringLiteral("Transition"), Qt::CaseInsensitive) == 0) {
				return b;
			}
		}
		return nullptr;
	};

	if (g_tbar_slider) {
		if (QPushButton *b = match_in_widget(g_tbar_slider->parentWidget())) {
			return b;
		}
		if (g_tbar_slider->parentWidget()) {
			if (QPushButton *b = match_in_widget(g_tbar_slider->parentWidget()->parentWidget())) {
				return b;
			}
		}
	}
	QWidget *main_win = static_cast<QWidget *>(obs_frontend_get_main_window());
	return match_in_widget(main_win);
}
#endif

static void refresh_studio_mode_state_on_ui_thread(void)
{
	if (!g_main_win) {
		g_main_win = static_cast<QWidget *>(obs_frontend_get_main_window());
	}
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

static void execute_tbar_on_ui_thread(int pos_1024, bool do_set, bool do_release, uint64_t epoch);

static void flush_deferred_tbar_on_ui_thread(void)
{
	if (!g_pending_deferred_tbar.active) {
		return;
	}
	obs_source_t *tr = obs_get_output_source(0);
	const bool active_tr = tr ? obs_transition_is_active(tr) : false;
	if (tr) {
		obs_source_release(tr);
	}
	if (g_tbar_awaiting_transition_stop && active_tr) {
		return;
	}
	g_tbar_awaiting_transition_stop = false;
	const mfd_deferred_tbar_cmd cmd = g_pending_deferred_tbar;
	g_pending_deferred_tbar.active = false;
	execute_tbar_on_ui_thread(cmd.pos_1024, cmd.do_set, cmd.do_release, cmd.epoch);
}

static void execute_tbar_on_ui_thread(int pos_1024, bool do_set, bool do_release, uint64_t epoch)
{
	if (!obs_frontend_preview_program_mode_active()) {
		g_studio_mode_active.store(false, std::memory_order_relaxed);
		g_tbar_slider.clear();
		g_slider_tbar_found.store(false, std::memory_order_relaxed);
		g_tbar_ui_throw_active = false;
		g_tbar_awaiting_transition_stop = false;
		g_pending_deferred_tbar.active = false;
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

	/*
	 * Per-transition UI-thread release latch:
	 * Between one manual transition start and the next unpark (next epoch),
	 * at most ONE release may execute and zero slider writes may occur after release.
	 */
	if (epoch <= g_tbar_ui_last_released_epoch) {
		return;
	}
	if (!do_release && !g_tbar_ui_throw_active && epoch < g_tbar_throw_epoch.load(std::memory_order_acquire)) {
		return;
	}

	/*
	 * Guard against OBSBasic::TransitionToScene stillTransitioning race:
	 * If a previous completed throw is still finishing its TransitionStopped swap,
	 * coalesce this new throw's start until TransitionStopped finishes.
	 */
	if (!g_tbar_ui_throw_active && (do_set || do_release)) {
		obs_source_t *tr_chk = obs_get_output_source(0);
		const bool tr_busy = g_tbar_awaiting_transition_stop || (tr_chk && obs_transition_is_active(tr_chk));
		if (tr_chk) {
			obs_source_release(tr_chk);
		}
		if (tr_busy) {
			if (!g_pending_deferred_tbar.active || epoch > g_pending_deferred_tbar.epoch) {
				g_pending_deferred_tbar = {true, pos_1024, do_set, do_release, epoch};
			} else if (epoch == g_pending_deferred_tbar.epoch && !g_pending_deferred_tbar.do_release) {
				g_pending_deferred_tbar.pos_1024 = pos_1024;
				g_pending_deferred_tbar.do_set = g_pending_deferred_tbar.do_set || do_set;
				g_pending_deferred_tbar.do_release = g_pending_deferred_tbar.do_release || do_release;
			}
			return;
		}
	}

	const bool dbg = g_tbar_debug_log.load(std::memory_order_relaxed) ||
			 qEnvironmentVariableIsSet("MFD_TBAR_DEBUG");
	if (dbg && (do_set || do_release)) {
		struct mfd_engine *eng_dbg = mfd_engine_get();
		int raw_cc = -1;
		float raw_norm = -1.0f;
		const char *st_str = "NO_ENGINE";
		float peak = -1.0f;
		int pol_rev = 0;
		if (eng_dbg) {
			mfd_engine_lock(eng_dbg);
			raw_cc = eng_dbg->has_last_midi ? (int)eng_dbg->last_midi_decoded.raw_value : -1;
			raw_norm = eng_dbg->fader_slots[0].last_raw_norm;
			st_str = takeover_state_str(eng_dbg->fader_slots[0].takeover.state);
			peak = eng_dbg->fader_slots[0].takeover.parked_peak;
			pol_rev = (int)eng_dbg->fader_slots[0].takeover.polarity_reversed;
			mfd_engine_unlock(eng_dbg);
		}
		obs_source_t *tr_dbg = obs_get_output_source(0);
		float tr_t = tr_dbg ? obs_transition_get_time(tr_dbg) : -1.0f;
		if (tr_dbg) {
			obs_source_release(tr_dbg);
		}
		obs_source_t *prev_s = obs_frontend_get_current_preview_scene();
		obs_source_t *prog_s = obs_frontend_get_current_scene();
		obs_log(LOG_INFO,
			"TBAR DEBUG: write=%d(pos=%d slider_before=%d) release=%d raw_cc=%d raw_norm=%.4f state=%s peak=%.4f pol_rev=%d tr_time=%.4f epoch=%llu preview='%s' program='%s'",
			(int)do_set, pos_1024, g_tbar_slider->value(), (int)do_release, raw_cc, raw_norm, st_str, peak,
			pol_rev, tr_t, static_cast<unsigned long long>(epoch),
			(prev_s && obs_source_get_name(prev_s)) ? obs_source_get_name(prev_s) : "",
			(prog_s && obs_source_get_name(prog_s)) ? obs_source_get_name(prog_s) : "");
		if (prev_s) {
			obs_source_release(prev_s);
		}
		if (prog_s) {
			obs_source_release(prog_s);
		}
	}

	if (do_set) {
		/*
		 * Sync both OBSBasic::tBar QSlider value and OBS transition position so that
		 * obs_frontend_release_tbar() sees tBar->value() >= 921 at completion and does not
		 * snap back to the old scene (OBSStudioAPI.cpp:115 vs OBSBasic_Transitions.cpp:752).
		 */
		g_tbar_ui_throw_active = true;
		g_tbar_slider->blockSignals(true);
		g_tbar_slider->setValue(pos_1024);
		g_tbar_slider->blockSignals(false);
		obs_frontend_set_tbar_position(pos_1024);
		g_tbar_set_count.fetch_add(1, std::memory_order_relaxed);
		g_tbar_writes_since_release.fetch_add(1, std::memory_order_relaxed);
		if (g_tbar_release_count.load(std::memory_order_relaxed) >= 1) {
			g_tbar_post_first_release_writes.fetch_add(1, std::memory_order_relaxed);
		}
	}

	if (do_release) {
		g_tbar_ui_last_released_epoch = epoch;
		g_tbar_ui_throw_active = false;
		obs_frontend_release_tbar();
		obs_source_t *tr_rel = obs_get_output_source(0);
		if (tr_rel) {
			obs_transition_set_manual_torque(tr_rel, 0.0f, 0.0f);
			if (obs_transition_is_active(tr_rel)) {
				g_tbar_awaiting_transition_stop = true;
			}
			obs_source_release(tr_rel);
		}
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

static bool capture_source_screenshot_by_uuid_srgb(const char *source_uuid, float *out_srgb_r, float *out_srgb_g,
						   float *out_srgb_b)
{
	if (!source_uuid || source_uuid[0] == '\0') {
		return false;
	}
	obs_data_t *req = obs_data_create();
	obs_data_set_string(req, "sourceUuid", source_uuid);
	obs_data_set_string(req, "imageFormat", "bmp");
	obs_data_set_int(req, "imageWidth", 64);
	obs_data_set_int(req, "imageHeight", 64);

	struct obs_websocket_request_response *resp = obs_websocket_call_request("GetSourceScreenshot", req);
	obs_data_release(req);

	if (!resp) {
		obs_log(LOG_ERROR, "GetSourceScreenshot returned null response for uuid '%s'", source_uuid);
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
					const int w = img.width();
					const int h = img.height();
					for (int y = 0; y < h; ++y) {
						for (int x = 0; x < w; ++x) {
							const QColor c = img.pixelColor(x, y);
							sum_r += c.redF();
							sum_g += c.greenF();
							sum_b += c.blueF();
						}
					}
					const double total = static_cast<double>(w) * static_cast<double>(h);
					if (out_srgb_r) {
						*out_srgb_r = static_cast<float>(sum_r / total);
					}
					if (out_srgb_g) {
						*out_srgb_g = static_cast<float>(sum_g / total);
					}
					if (out_srgb_b) {
						*out_srgb_b = static_cast<float>(sum_b / total);
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
		obs_log(LOG_ERROR, "GetSourceScreenshot failed for uuid '%s': status=%u comment=%s", source_uuid,
			resp->status_code, resp->comment ? resp->comment : "");
	}

	obs_websocket_request_response_free(resp);
	return ok;
}

static const char *selftest_program_tap_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return "Midi Fadester — Self-Test Program Tap";
}

static void *selftest_program_tap_create(obs_data_t *settings, obs_source_t *source)
{
	UNUSED_PARAMETER(settings);
	return source;
}

static void selftest_program_tap_destroy(void *data)
{
	UNUSED_PARAMETER(data);
}

static uint32_t selftest_program_tap_get_width(void *data)
{
	UNUSED_PARAMETER(data);
	obs_source_t *tr = obs_get_output_source(0);
	uint32_t w = tr ? obs_source_get_width(tr) : 1920U;
	if (tr) {
		obs_source_release(tr);
	}
	return w ? w : 1920U;
}

static uint32_t selftest_program_tap_get_height(void *data)
{
	UNUSED_PARAMETER(data);
	obs_source_t *tr = obs_get_output_source(0);
	uint32_t h = tr ? obs_source_get_height(tr) : 1080U;
	if (tr) {
		obs_source_release(tr);
	}
	return h ? h : 1080U;
}

static void selftest_program_tap_video_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(effect);
	obs_source_t *tr = obs_get_output_source(0);
	if (tr) {
		obs_source_video_render(tr);
		obs_source_release(tr);
	}
}

static bool g_selftest_tap_registered = false;

static void register_selftest_program_tap_once(void)
{
	if (g_selftest_tap_registered) {
		return;
	}
	struct obs_source_info info = {};
	info.id = "mfd_selftest_program_tap";
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_SRGB | OBS_SOURCE_CAP_DISABLED;
	info.get_name = selftest_program_tap_get_name;
	info.create = selftest_program_tap_create;
	info.destroy = selftest_program_tap_destroy;
	info.get_width = selftest_program_tap_get_width;
	info.get_height = selftest_program_tap_get_height;
	info.video_render = selftest_program_tap_video_render;
	obs_register_source(&info);
	g_selftest_tap_registered = true;
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
	obs_scene_t *tbar_red_scene = nullptr;
	obs_scene_t *tbar_blue_scene = nullptr;
	obs_source_t *src_a = nullptr;
	obs_source_t *src_b = nullptr;
	obs_source_t *tbar_src_red = nullptr;
	obs_source_t *tbar_src_blue = nullptr;

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
				"__mfd_selftest_source_a__",     "__mfd_selftest_source_b__",
				"__mfd_selftest_source_a__ 2",   "__mfd_selftest_source_b__ 2",
				"__mfd_selftest_scene__",        "__mfd_selftest_scene__ 2",
				"__mfd_selftest_scene_b__",      "__mfd_selftest_scene_b__ 2",
				"__mfd_selftest_tbar_red__",     "__mfd_selftest_tbar_red__ 2",
				"__mfd_selftest_tbar_blue__",    "__mfd_selftest_tbar_blue__ 2",
				"__mfd_selftest_tbar_red_src__", "__mfd_selftest_tbar_blue_src__",
				"__mfd_selftest_program_tap__",  "__mfd_selftest_program_tap__ 2",
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
			tbar_red_scene = obs_scene_create("__mfd_selftest_tbar_red__");
			tbar_blue_scene = obs_scene_create("__mfd_selftest_tbar_blue__");

			obs_data_t *red_settings = obs_data_create();
			obs_data_set_int(red_settings, "color", 0xFF0000FFLL);
			obs_data_set_int(red_settings, "width", 1920);
			obs_data_set_int(red_settings, "height", 1080);
			src_a = obs_source_create("color_source", "__mfd_selftest_source_a__", red_settings, nullptr);
			tbar_src_red = obs_source_create("color_source", "__mfd_selftest_tbar_red_src__", red_settings,
							 nullptr);
			obs_data_release(red_settings);

			obs_data_t *blue_settings = obs_data_create();
			obs_data_set_int(blue_settings, "color", 0xFFFF0000LL);
			obs_data_set_int(blue_settings, "width", 1920);
			obs_data_set_int(blue_settings, "height", 1080);
			src_b = obs_source_create("color_source", "__mfd_selftest_source_b__", blue_settings, nullptr);
			tbar_src_blue = obs_source_create("color_source", "__mfd_selftest_tbar_blue_src__",
							  blue_settings, nullptr);
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
			if (tbar_red_scene && tbar_src_red) {
				obs_scene_add(tbar_red_scene, tbar_src_red);
			}
			if (tbar_blue_scene && tbar_src_blue) {
				obs_scene_add(tbar_blue_scene, tbar_src_blue);
			}

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

	set_fader1_and_wait(1.0);
	float r1 = 0.0f, g1 = 0.0f, b1 = 0.0f, srgb1 = 0.0f;
	bool ss1_ok = capture_source_screenshot_avg_rgb(program_scene_name, &r1, &g1, &b1, &srgb1);
	float bright_blue = r1 + g1 + b1;

	obs_source_t *flt_a = src_a ? obs_source_get_filter_by_name(src_a, MFD_INTERNAL_OPACITY_FILTER_NAME) : nullptr;
	obs_source_t *flt_b = src_b ? obs_source_get_filter_by_name(src_b, MFD_INTERNAL_OPACITY_FILTER_NAME) : nullptr;
	float read_a_end = flt_a ? mfd_layer_filter_get_opacity(flt_a) : -1.0f;
	float read_b_end = flt_b ? mfd_layer_filter_get_opacity(flt_b) : -1.0f;

	set_fader1_and_wait(0.0);
	float r0 = 0.0f, g0 = 0.0f, b0 = 0.0f, srgb0 = 0.0f;
	bool ss0_ok = capture_source_screenshot_avg_rgb(program_scene_name, &r0, &g0, &b0, &srgb0);
	float bright_red = r0 + g0 + b0;
	float min_source_brightness = std::min(bright_red, bright_blue);

	/*
	 * Check (2): Program output GetSourceScreenshot at t = 0.25, 0.50, 0.75, 0.92, 0.97
	 * driven through the live MIDI queue + takeover + smoother + rail check on fader_1 (Ch 1, 14-bit CC 7/39).
	 * Temporarily pause WinMM input so eng->midi_queue remains strictly single-producer.
	 */
	char prev_midi_for_ss[MFD_MIDI_DEV_NAME_MAX] = {0};
	bool reopen_midi_after_ss = false;
	QMetaObject::invokeMethod(
		main_win,
		[&]() {
			if (eng->midi_dev.is_open) {
				std::snprintf(prev_midi_for_ss, sizeof(prev_midi_for_ss), "%s",
					      eng->midi_dev.opened_name);
				mfd_winmm_close(&eng->midi_dev);
				reopen_midi_after_ss = true;
			}
		},
		Qt::BlockingQueuedConnection);
	std::this_thread::sleep_for(std::chrono::milliseconds(40));

	mfd_takeover_init(&eng->fader_slots[1].takeover, 0.0f, MFD_DEFAULT_PICKUP_THRESHOLD, false);
	mfd_takeover_set_two_rail_mode(&eng->fader_slots[1].takeover, true);
	eng->fader_slots[1].takeover.state = MFD_TAKEOVER_ENGAGED;
	mfd_smoother_reset(&eng->fader_slots[1].smoother, 0.0f);
	eng->fader_slots[1].last_applied_val = 0.0f;

	auto send_fader1_midi14_and_wait = [&](float t_norm) {
		int v14 = static_cast<int>(std::ceil(static_cast<double>(t_norm) * 16383.0));
		if (v14 < 0) {
			v14 = 0;
		} else if (v14 > 16383) {
			v14 = 16383;
		}
		const uint32_t msb = static_cast<uint32_t>((v14 >> 7) & 0x7F);
		const uint32_t lsb = static_cast<uint32_t>(v14 & 0x7F);
		const uint32_t msg_msb = 0xB0U | (7U << 8U) | (msb << 16U);
		const uint32_t msg_lsb = 0xB0U | (39U << 8U) | (lsb << 16U);
		mfd_midi_queue_push(&eng->midi_queue, msg_msb, 0U);
		mfd_midi_queue_push(&eng->midi_queue, msg_lsb, 0U);
		std::this_thread::sleep_for(std::chrono::milliseconds(220));
	};

	const float test_ts[5] = {0.25f, 0.50f, 0.75f, 0.92f, 0.97f};
	bool screenshot_all_pass = ss0_ok && ss1_ok;
	float read_a_mid = -1.0f;
	float read_b_mid = -1.0f;

	for (int i = 0; i < 5; ++i) {
		const float t = test_ts[i];
		send_fader1_midi14_and_wait(t);
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
		const bool is_parked = (eng->fader_slots[1].takeover.state == MFD_TAKEOVER_PARKED);
		const bool expected_parked = (t >= MFD_TWO_RAIL_UPPER_PARK_THRESHOLD);
		const bool parked_ok = (is_parked == expected_parked);
		const bool step_ok = mix_ok && bright_ok && parked_ok;
		if (!step_ok) {
			screenshot_all_pass = false;
		}
		obs_log(step_ok ? LOG_INFO : LOG_ERROR,
			"SCREENSHOT CHECK t=%.2f [%s]: program='%s' avg_rgb=(%.4f, %.4f, %.4f) expected=(%.4f, %.4f, %.4f) max_err=%.4f brightness=%.4f srgb_brightness=%.4f (src_red=%.4f src_blue=%.4f) mix_within_2pct=%d bright_no_dip=%d parked=%d(expected=%d)",
			t, step_ok ? "PASS" : "FAIL", program_scene_name, avg_r, avg_g, avg_b, exp_r, exp_g, exp_b,
			std::max({err_r, err_g, err_b}), brightness, srgb_bright, bright_red, bright_blue, (int)mix_ok,
			(int)bright_ok, (int)is_parked, (int)expected_parked);
	}

	/*
	 * Check (2b): Live libobs "reorder", "item_remove", and "item_add" signal verification (zero timers):
	 *   1. Move src_a (Left / Red) to OBS_ORDER_MOVE_TOP so Left is ABOVE Right (Blue) in test_scene.
	 *   2. Set fader_1 = 0.25 and capture GetSourceScreenshot -> must render (0.75 Red, 0.25 Blue) with zero dip.
	 *   3. Remove src_b from test_scene -> get_state immediately reports available=false and
	 *      "<src_b> is no longer in scene <program_scene_name>." via libobs "item_remove" signal.
	 *   4. Re-add src_b to test_scene -> get_state immediately reports available=true, reason="" via "item_add".
	 */
	QMetaObject::invokeMethod(
		main_win,
		[&]() {
			obs_sceneitem_t *item_a = test_scene ? obs_scene_find_source(test_scene, src_a_actual_name)
							     : nullptr;
			if (item_a) {
				obs_sceneitem_set_order(item_a, OBS_ORDER_MOVE_TOP);
			}
		},
		Qt::BlockingQueuedConnection);

	set_fader1_and_wait(0.25);
	float inv_r = 0.0f, inv_g = 0.0f, inv_b = 0.0f, inv_srgb = 0.0f;
	bool inv_cap_ok = capture_source_screenshot_avg_rgb(program_scene_name, &inv_r, &inv_g, &inv_b, &inv_srgb);
	bool inv_order_ok = eng->fader_slots[1].a_is_above_b && inv_cap_ok && (std::fabs(inv_r - 0.75f) <= 0.02f) &&
			    (std::fabs(inv_g - 0.0f) <= 0.02f) && (std::fabs(inv_b - 0.25f) <= 0.02f) &&
			    ((inv_r + inv_g + inv_b) >= min_source_brightness - 0.008f);

	QMetaObject::invokeMethod(
		main_win,
		[&]() {
			obs_sceneitem_t *item_b = test_scene ? obs_scene_find_source(test_scene, src_b_actual_name)
							     : nullptr;
			if (item_b) {
				obs_sceneitem_remove(item_b);
			}
		},
		Qt::BlockingQueuedConnection);

	bool rm_unavail_ok = false;
	char expected_rm_reason[256] = {0};
	std::snprintf(expected_rm_reason, sizeof(expected_rm_reason), "%s is no longer in scene %s.", src_b_actual_name,
		      program_scene_name);
	{
		obs_data_t *gs_req = obs_data_create();
		obs_data_t *gs_res = call_real_ws_vendor_request("get_state", gs_req);
		if (gs_res) {
			obs_data_array_t *arr = obs_data_get_array(gs_res, "slots");
			obs_data_t *s1 = arr ? obs_data_array_item(arr, 1) : nullptr;
			if (s1) {
				const bool av = obs_data_get_bool(s1, "available");
				const char *rsn = obs_data_get_string(s1, "reason");
				rm_unavail_ok = (!av) && rsn && (std::strcmp(rsn, expected_rm_reason) == 0);
				obs_data_release(s1);
			}
			if (arr) {
				obs_data_array_release(arr);
			}
			obs_data_release(gs_res);
		}
		obs_data_release(gs_req);
	}

	QMetaObject::invokeMethod(
		main_win,
		[&]() {
			if (test_scene && src_b) {
				obs_scene_add(test_scene, src_b);
			}
		},
		Qt::BlockingQueuedConnection);

	bool readd_avail_ok = false;
	{
		obs_data_t *gs_req = obs_data_create();
		obs_data_t *gs_res = call_real_ws_vendor_request("get_state", gs_req);
		if (gs_res) {
			obs_data_array_t *arr = obs_data_get_array(gs_res, "slots");
			obs_data_t *s1 = arr ? obs_data_array_item(arr, 1) : nullptr;
			if (s1) {
				const bool av = obs_data_get_bool(s1, "available");
				const char *rsn = obs_data_get_string(s1, "reason");
				readd_avail_ok = av && rsn && (rsn[0] == '\0') && (!eng->fader_slots[1].a_is_above_b);
				obs_data_release(s1);
			}
			if (arr) {
				obs_data_array_release(arr);
			}
			obs_data_release(gs_res);
		}
		obs_data_release(gs_req);
	}

	const bool scene_signals_ok = inv_order_ok && rm_unavail_ok && readd_avail_ok;
	if (!scene_signals_ok) {
		screenshot_all_pass = false;
	}
	obs_log(scene_signals_ok ? LOG_INFO : LOG_ERROR,
		"SCENE SIGNALS & STACK INVERSION CHECK [%s]: inv_order_ok=%d (avg_rgb=%.4f,%.4f,%.4f) rm_unavail_ok=%d ('%s') readd_avail_ok=%d",
		scene_signals_ok ? "PASS" : "FAIL", (int)inv_order_ok, inv_r, inv_g, inv_b, (int)rm_unavail_ok,
		expected_rm_reason, (int)readd_avail_ok);

	if (reopen_midi_after_ss && prev_midi_for_ss[0] != '\0') {
		QMetaObject::invokeMethod(
			main_win, [&]() { mfd_engine_reconnect_midi(eng, prev_midi_for_ss); },
			Qt::BlockingQueuedConnection);
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

	/* Verify get_state exposes build identity, parked=true, repeated "Finish T-bar" is a one-shot no-op, and nudge_fader unparks */
	std::this_thread::sleep_for(std::chrono::milliseconds(50));
	int rel_after_first_finish = g_tbar_release_count.load(std::memory_order_relaxed);
	bool ws_parked_true = false;
	bool ws_build_ok = false;
	char ws_ver[64] = {0};
	char ws_commit[64] = {0};
	char ws_built_at[64] = {0};
	obs_data_t *gs_res = call_real_ws_vendor_request("get_state", nullptr);
	if (gs_res) {
		const char *v = obs_data_get_string(gs_res, "version");
		if (v) {
			std::snprintf(ws_ver, sizeof(ws_ver), "%s", v);
		}
		obs_data_t *bobj = obs_data_get_obj(gs_res, "build");
		if (bobj) {
			const char *cm = obs_data_get_string(bobj, "commit");
			const char *ba = obs_data_get_string(bobj, "built_at");
			if (cm) {
				std::snprintf(ws_commit, sizeof(ws_commit), "%s", cm);
			}
			if (ba) {
				std::snprintf(ws_built_at, sizeof(ws_built_at), "%s", ba);
			}
			obs_data_release(bobj);
		}
		ws_build_ok = (std::strcmp(ws_ver, PLUGIN_VERSION) == 0) &&
			      (std::strcmp(ws_commit, PLUGIN_GIT_COMMIT) == 0) &&
			      (std::strcmp(ws_built_at, PLUGIN_BUILT_AT) == 0) && (ws_commit[0] != '\0') &&
			      (ws_built_at[0] != '\0');
		obs_log(ws_build_ok ? LOG_INFO : LOG_ERROR,
			"BUILD IDENTITY CHECK [%s]: version='%s' build={ commit='%s', built_at='%s' }",
			ws_build_ok ? "PASS" : "FAIL", ws_ver, ws_commit, ws_built_at);

		obs_data_array_t *gs_slots = obs_data_get_array(gs_res, "slots");
		if (gs_slots) {
			obs_data_t *gs_s0 = obs_data_array_item(gs_slots, 0);
			if (gs_s0) {
				ws_parked_true = obs_data_get_bool(gs_s0, "parked");
				obs_data_release(gs_s0);
			}
			obs_data_array_release(gs_slots);
		}
		obs_data_release(gs_res);
	}

	req = obs_data_create();
	obs_data_set_string(req, "slot", "crossfader");
	obs_data_set_double(req, "value", 1.0);
	obs_data_set_bool(req, "release", true);
	res = call_real_ws_vendor_request("set_fader_value", req);
	if (res) {
		obs_data_release(res);
	}
	obs_data_release(req);
	std::this_thread::sleep_for(std::chrono::milliseconds(50));
	bool finish_tbar_oneshot_ok = (g_tbar_release_count.load(std::memory_order_relaxed) == rel_after_first_finish);

	/* Allow the vendor API T-bar release transition to finish before nudge + loopMIDI sweeps */
	std::this_thread::sleep_for(std::chrono::milliseconds(550));

	req = obs_data_create();
	obs_data_set_string(req, "slot", "crossfader");
	obs_data_set_double(req, "delta", -0.05);
	res = call_real_ws_vendor_request("nudge_fader", req);
	bool nudge_val_ok = res && (std::fabs(obs_data_get_double(res, "value") - 0.05) < 1e-4);
	if (res) {
		obs_data_release(res);
	}
	obs_data_release(req);

	bool ws_unparked_false = false;
	gs_res = call_real_ws_vendor_request("get_state", nullptr);
	if (gs_res) {
		obs_data_array_t *gs_slots = obs_data_get_array(gs_res, "slots");
		if (gs_slots) {
			obs_data_t *gs_s0 = obs_data_array_item(gs_slots, 0);
			if (gs_s0) {
				ws_unparked_false = !obs_data_get_bool(gs_s0, "parked");
				obs_data_release(gs_s0);
			}
			obs_data_array_release(gs_slots);
		}
		obs_data_release(gs_res);
	}

	req = obs_data_create();
	obs_data_set_string(req, "slot", "crossfader");
	obs_data_set_double(req, "value", 1.0);
	obs_data_set_bool(req, "release", true);
	res = call_real_ws_vendor_request("set_fader_value", req);
	if (res) {
		obs_data_release(res);
	}
	obs_data_release(req);
	std::this_thread::sleep_for(std::chrono::milliseconds(550));

	obs_log((ws_parked_true && finish_tbar_oneshot_ok && nudge_val_ok && ws_unparked_false) ? LOG_INFO : LOG_ERROR,
		"WS RAIL CHECK [%s]: get_state_parked=%d finish_tbar_oneshot=%d nudge_after_throw_ok=%d get_state_unparked=%d",
		(ws_parked_true && finish_tbar_oneshot_ok && nudge_val_ok && ws_unparked_false) ? "PASS" : "FAIL",
		(int)ws_parked_true, (int)finish_tbar_oneshot_ok, (int)nudge_val_ok, (int)ws_unparked_false);

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
		mfd_tbar_reset_release_latch();
		mfd_takeover_sync_external(&eng->fader_slots[0].takeover, 0.50f);
		mfd_smoother_reset(&eng->fader_slots[0].smoother, 0.50f);
		eng->fader_slots[0].tbar_state = MFD_TBAR_THROWING;
		eng->fader_slots[0].last_applied_val = 0.50f;
		g_last_pos_1024.store(512, std::memory_order_relaxed);
		const uint64_t ep_p1a = g_tbar_throw_epoch.load(std::memory_order_acquire);
		QMetaObject::invokeMethod(
			main_win, [&]() { execute_tbar_on_ui_thread(512, true, false, ep_p1a); },
			Qt::BlockingQueuedConnection);

		/* Part 1a: Sweep 0..30 (0.00..0.236, away from 0.50) -> soft takeover must stay WAITING and T-bar must stay at 512 */
		int rc_pre = QProcess::execute(midi_send_exe, QStringList() << QString::fromUtf8(loopmidi_port) << "1"
									    << "15"
									    << "60"
									    << "0"
									    << "30");
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
		bool pre_waiting_ok = (rc_pre == 0) && (eng->fader_slots[0].takeover.state == MFD_TAKEOVER_WAITING) &&
				      (eng->last_midi_decoded.raw_value == 30);
		int slider_pre = -1;
		QMetaObject::invokeMethod(
			main_win, [&]() { slider_pre = g_tbar_slider ? g_tbar_slider->value() : -1; },
			Qt::BlockingQueuedConnection);
		bool pre_slider_held = (slider_pre == 512);

		/* Part 1b: Full sweep 0..127 over 450 ms -> soft takeover engages at ~64 (0.50), T-bar follows within smoothing window, completes in 3% rail zone and parks */
		mfd_takeover_sync_external(&eng->fader_slots[0].takeover, 0.50f);
		mfd_smoother_reset(&eng->fader_slots[0].smoother, 0.50f);
		eng->fader_slots[0].tbar_state = MFD_TBAR_THROWING;
		eng->fader_slots[0].last_applied_val = 0.50f;
		g_last_pos_1024.store(512, std::memory_order_relaxed);
		QMetaObject::invokeMethod(
			main_win, [&]() { execute_tbar_on_ui_thread(512, true, false, ep_p1a); },
			Qt::BlockingQueuedConnection);

		QProcess sweep_proc;
		sweep_proc.start(midi_send_exe, QStringList() << QString::fromUtf8(loopmidi_port) << "1"
							      << "15"
							      << "450"
							      << "0"
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
		mfd_tbar_reset_release_latch();
		mfd_takeover_init(&eng->fader_slots[0].takeover, 0.0f, MFD_DEFAULT_PICKUP_THRESHOLD, true);
		eng->fader_slots[0].takeover.state = MFD_TAKEOVER_ENGAGED;
		mfd_smoother_reset(&eng->fader_slots[0].smoother, 0.0f);
		eng->fader_slots[0].tbar_state = MFD_TBAR_IDLE;
		eng->fader_slots[0].last_applied_val = 0.0f;
		eng->fader_slots[0].tbar_midi_active = false;
		g_last_pos_1024.store(0, std::memory_order_relaxed);
		g_obs_manual_started.store(0, std::memory_order_relaxed);
		g_obs_manual_finished.store(0, std::memory_order_relaxed);
		g_obs_manual_cancelled.store(0, std::memory_order_relaxed);
		g_tbar_release_count.store(0, std::memory_order_relaxed);
		g_tbar_writes_since_release.store(0, std::memory_order_relaxed);
		g_tbar_post_first_release_writes.store(0, std::memory_order_relaxed);
		g_tbar_writes_before_unpark.store(0, std::memory_order_relaxed);
		g_tbar_awaiting_unpark.store(false, std::memory_order_relaxed);

		int rc_c2_d1 = QProcess::execute(midi_send_exe, QStringList() << QString::fromUtf8(loopmidi_port) << "1"
									      << "15"
									      << "400"
									      << "0"
									      << "127");
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
									      << "15"
									      << "400"
									      << "127"
									      << "0");
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
		 * Case (3): Calibrated short-travel fader [0, 125] (R-MIDI-5 + R-TBAR-SM, zero timers):
		 * - Calibrate crossfader to [0, 125] via set_slot; a fader that mechanically stops at 125 is at its 1.0 rail at 125.
		 * - Sweep 0 -> 125, dwell, then 125 -> 0: assert 1 started + 1 finished + 1 release per direction, 0 post-completion writes.
		 * - Then send CC 127 (> cal_max 125) and verify auto-widening immediately updates and persists cal_max = 127 in get_state.
		 */
		obs_data_t *cal_req = obs_data_create();
		obs_data_set_string(cal_req, "slot", "crossfader");
		obs_data_set_int(cal_req, "cal_min", 0);
		obs_data_set_int(cal_req, "cal_max", 125);
		obs_data_t *cal_res = call_real_ws_vendor_request("set_slot", cal_req);
		bool cal_set_125_ok = cal_res && (obs_data_get_int(cal_res, "cal_min") == 0) &&
				      (obs_data_get_int(cal_res, "cal_max") == 125);
		if (cal_res) {
			obs_data_release(cal_res);
		}
		obs_data_release(cal_req);

		mfd_tbar_reset_release_latch();
		mfd_takeover_init(&eng->fader_slots[0].takeover, 0.0f, MFD_DEFAULT_PICKUP_THRESHOLD, true);
		eng->fader_slots[0].takeover.state = MFD_TAKEOVER_ENGAGED;
		mfd_smoother_reset(&eng->fader_slots[0].smoother, 0.0f);
		eng->fader_slots[0].tbar_state = MFD_TBAR_IDLE;
		eng->fader_slots[0].last_applied_val = 0.0f;
		eng->fader_slots[0].tbar_midi_active = false;
		g_last_pos_1024.store(0, std::memory_order_relaxed);
		g_obs_manual_started.store(0, std::memory_order_relaxed);
		g_obs_manual_finished.store(0, std::memory_order_relaxed);
		g_tbar_release_count.store(0, std::memory_order_relaxed);
		g_tbar_writes_since_release.store(0, std::memory_order_relaxed);
		g_tbar_post_first_release_writes.store(0, std::memory_order_relaxed);
		g_tbar_writes_before_unpark.store(0, std::memory_order_relaxed);
		g_tbar_awaiting_unpark.store(false, std::memory_order_relaxed);

		int rc_c3_d1 = QProcess::execute(midi_send_exe, QStringList() << QString::fromUtf8(loopmidi_port) << "1"
									      << "15"
									      << "400"
									      << "0"
									      << "125");
		/* Dwell at calibrated 125 rail (550 ms) */
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
									      << "15"
									      << "400"
									      << "125"
									      << "0");
		std::this_thread::sleep_for(std::chrono::milliseconds(550));

		int c3_d2_started = g_obs_manual_started.load(std::memory_order_relaxed);
		int c3_d2_finished = g_obs_manual_finished.load(std::memory_order_relaxed);
		int c3_d2_releases = g_tbar_release_count.load(std::memory_order_relaxed);
		int c3_d2_post_writes = g_tbar_writes_since_release.load(std::memory_order_relaxed);
		int c3_writes_before_unpark = g_tbar_writes_before_unpark.load(std::memory_order_relaxed);
		bool c3_d2_parked = (eng->fader_slots[0].takeover.state == MFD_TAKEOVER_PARKED);
		float c3_d2_peak = eng->fader_slots[0].takeover.parked_peak;

		/* Auto-widen check: reporting 127 beyond stored cal_max=125 widens to [0, 127] and persists */
		int rc_c3_widen = QProcess::execute(midi_send_exe, QStringList()
									   << QString::fromUtf8(loopmidi_port) << "1"
									   << "15"
									   << "200"
									   << "0"
									   << "127");
		std::this_thread::sleep_for(std::chrono::milliseconds(550));
		int c3_widened_max = -1;
		obs_data_t *c3_gs = call_real_ws_vendor_request("get_state", nullptr);
		if (c3_gs) {
			obs_data_array_t *c3_arr = obs_data_get_array(c3_gs, "slots");
			if (c3_arr) {
				obs_data_t *c3_s0 = obs_data_array_item(c3_arr, 0);
				if (c3_s0) {
					c3_widened_max = static_cast<int>(obs_data_get_int(c3_s0, "cal_max"));
					obs_data_release(c3_s0);
				}
				obs_data_array_release(c3_arr);
			}
			obs_data_release(c3_gs);
		}

		bool case3_ok = cal_set_125_ok && (rc_c3_d1 == 0) && (rc_c3_d2 == 0) && (rc_c3_widen == 0) &&
				(c3_d1_started == 1) && (c3_d1_finished == 1) && (c3_d1_releases == 1) &&
				(c3_d1_post_writes == 0) && c3_d1_parked && (std::fabs(c3_d1_peak - 1.0f) < 1e-4f) &&
				(c3_d2_started == 1) && (c3_d2_finished == 1) && (c3_d2_releases == 1) &&
				(c3_d2_post_writes == 0) && (c3_writes_before_unpark == 0) && c3_d2_parked &&
				(c3_widened_max == 127);

		obs_log(case3_ok ? LOG_INFO : LOG_ERROR,
			"LOOPMIDI CASE 3 (calibrated [0,125] 0->125->0 + auto-widen->127) [%s]: cal_set_125=%d dir1(started=%d finished=%d releases=%d parked=%d peak=%.4f post_writes=%d) dir2(started=%d finished=%d releases=%d parked=%d peak=%.4f post_writes=%d writes_before_unpark=%d) widened_cal_max=%d",
			case3_ok ? "PASS" : "FAIL", (int)cal_set_125_ok, c3_d1_started, c3_d1_finished, c3_d1_releases,
			(int)c3_d1_parked, c3_d1_peak, c3_d1_post_writes, c3_d2_started, c3_d2_finished, c3_d2_releases,
			(int)c3_d2_parked, c3_d2_peak, c3_d2_post_writes, c3_writes_before_unpark, c3_widened_max);

		/*
		 * Case (4): Rail jitter 127/126/127 every 100 ms for 3.0 s (30 steps) after a 0 -> 127 throw,
		 * with full TBAR DEBUG logging of every slider write and every release.
		 * Asserts: 1 started, 1 finished, 0 releases after the first, 0 slider writes after the first release,
		 * and Preview/Program ending swapped on the exact expected scenes.
		 */
		auto set_studio_scenes_and_wait = [&](obs_scene_t *prog_sc, obs_scene_t *prev_sc) {
			QMetaObject::invokeMethod(
				main_win,
				[&]() {
					if (prog_sc) {
						obs_frontend_set_current_scene(obs_scene_get_source(prog_sc));
					}
				},
				Qt::BlockingQueuedConnection);
			std::this_thread::sleep_for(std::chrono::milliseconds(650));
			QMetaObject::invokeMethod(
				main_win,
				[&]() {
					if (prev_sc) {
						obs_frontend_set_current_preview_scene(obs_scene_get_source(prev_sc));
					}
				},
				Qt::BlockingQueuedConnection);
			std::this_thread::sleep_for(std::chrono::milliseconds(60));
		};

		set_studio_scenes_and_wait(test_scene, test_scene_b);

		mfd_tbar_reset_release_latch();
		mfd_takeover_init(&eng->fader_slots[0].takeover, 0.0f, MFD_DEFAULT_PICKUP_THRESHOLD, true);
		eng->fader_slots[0].takeover.state = MFD_TAKEOVER_ENGAGED;
		mfd_smoother_reset(&eng->fader_slots[0].smoother, 0.0f);
		eng->fader_slots[0].tbar_state = MFD_TBAR_IDLE;
		eng->fader_slots[0].last_applied_val = 0.0f;
		eng->fader_slots[0].tbar_midi_active = false;
		g_last_pos_1024.store(0, std::memory_order_relaxed);
		g_obs_manual_started.store(0, std::memory_order_relaxed);
		g_obs_manual_finished.store(0, std::memory_order_relaxed);
		g_obs_manual_cancelled.store(0, std::memory_order_relaxed);
		g_tbar_release_count.store(0, std::memory_order_relaxed);
		g_tbar_writes_since_release.store(0, std::memory_order_relaxed);
		g_tbar_post_first_release_writes.store(0, std::memory_order_relaxed);
		g_tbar_writes_before_unpark.store(0, std::memory_order_relaxed);
		g_tbar_awaiting_unpark.store(false, std::memory_order_relaxed);
		g_tbar_debug_log.store(true, std::memory_order_relaxed);

		obs_log(LOG_INFO, "LOOPMIDI CASE 4 (0->127 + 127/126/127 x 100ms for 3s): starting...");
		int rc_c4_throw = QProcess::execute(midi_send_exe, QStringList()
									   << QString::fromUtf8(loopmidi_port) << "1"
									   << "15"
									   << "200"
									   << "0"
									   << "127");
		int rc_c4_jitter = QProcess::execute(midi_send_exe, QStringList()
									    << "--jitter"
									    << QString::fromUtf8(loopmidi_port) << "1"
									    << "15"
									    << "100"
									    << "30"
									    << "126"
									    << "127");
		std::this_thread::sleep_for(std::chrono::milliseconds(100));

		int c4_started = g_obs_manual_started.load(std::memory_order_relaxed);
		int c4_finished = g_obs_manual_finished.load(std::memory_order_relaxed);
		int c4_releases = g_tbar_release_count.load(std::memory_order_relaxed);
		int c4_post_releases = (c4_releases > 1) ? (c4_releases - 1) : 0;
		int c4_post_writes = std::max(g_tbar_writes_since_release.load(std::memory_order_relaxed),
					      g_tbar_post_first_release_writes.load(std::memory_order_relaxed));
		bool c4_parked = (eng->fader_slots[0].takeover.state == MFD_TAKEOVER_PARKED);

		char c4_prev_name[128] = {0};
		char c4_prog_name[128] = {0};
		QMetaObject::invokeMethod(
			main_win,
			[&]() {
				obs_source_t *ps = obs_frontend_get_current_preview_scene();
				obs_source_t *cs = obs_frontend_get_current_scene();
				if (ps && obs_source_get_name(ps)) {
					std::snprintf(c4_prev_name, sizeof(c4_prev_name), "%s",
						      obs_source_get_name(ps));
				}
				if (cs && obs_source_get_name(cs)) {
					std::snprintf(c4_prog_name, sizeof(c4_prog_name), "%s",
						      obs_source_get_name(cs));
				}
				if (ps) {
					obs_source_release(ps);
				}
				if (cs) {
					obs_source_release(cs);
				}
			},
			Qt::BlockingQueuedConnection);

		const char *exp_scene_a = (test_scene && obs_source_get_name(obs_scene_get_source(test_scene)))
						  ? obs_source_get_name(obs_scene_get_source(test_scene))
						  : "__mfd_selftest_scene__";
		const char *exp_scene_b = (test_scene_b && obs_source_get_name(obs_scene_get_source(test_scene_b)))
						  ? obs_source_get_name(obs_scene_get_source(test_scene_b))
						  : "__mfd_selftest_scene_b__";

		bool c4_scenes_ok = (std::strcmp(c4_prev_name, exp_scene_a) == 0) &&
				    (std::strcmp(c4_prog_name, exp_scene_b) == 0);
		bool case4_ok = (rc_c4_throw == 0) && (rc_c4_jitter == 0) && (c4_started == 1) && (c4_finished == 1) &&
				(c4_releases == 1) && (c4_post_releases == 0) && (c4_post_writes == 0) && c4_parked &&
				c4_scenes_ok;

		obs_log(case4_ok ? LOG_INFO : LOG_ERROR,
			"LOOPMIDI CASE 4 (127/126/127 x 100ms 3s) [%s]: started=%d finished=%d releases=%d post_first_releases=%d post_first_writes=%d parked=%d preview='%s'(exp='%s') program='%s'(exp='%s')",
			case4_ok ? "PASS" : "FAIL", c4_started, c4_finished, c4_releases, c4_post_releases,
			c4_post_writes, (int)c4_parked, c4_prev_name, exp_scene_a, c4_prog_name, exp_scene_b);

		/*
		 * Case (5): Rapid back-and-forth throws within OBS's ~530 ms manual-torque tail
		 * (0 -> 127, 250 ms pause, 127 -> 0, 250 ms pause, 0 -> 127 — matching video 17:41:58.815..17:42:01.948).
		 * Asserts: started == finished (never multiple finishes per start!) and Preview != Program on expected scenes.
		 */
		set_studio_scenes_and_wait(test_scene, test_scene_b);

		mfd_tbar_reset_release_latch();
		mfd_takeover_init(&eng->fader_slots[0].takeover, 0.0f, MFD_DEFAULT_PICKUP_THRESHOLD, true);
		eng->fader_slots[0].takeover.state = MFD_TAKEOVER_ENGAGED;
		mfd_smoother_reset(&eng->fader_slots[0].smoother, 0.0f);
		eng->fader_slots[0].tbar_state = MFD_TBAR_IDLE;
		eng->fader_slots[0].last_applied_val = 0.0f;
		eng->fader_slots[0].tbar_midi_active = false;
		g_last_pos_1024.store(0, std::memory_order_relaxed);
		g_obs_manual_started.store(0, std::memory_order_relaxed);
		g_obs_manual_finished.store(0, std::memory_order_relaxed);
		g_tbar_release_count.store(0, std::memory_order_relaxed);
		g_tbar_writes_since_release.store(0, std::memory_order_relaxed);
		g_tbar_post_first_release_writes.store(0, std::memory_order_relaxed);

		obs_log(LOG_INFO, "LOOPMIDI CASE 5 (rapid throws 0->127->0->127 every 200ms): starting...");
		int rc_c5 = QProcess::execute(midi_send_exe, QStringList() << "--rapid-throws"
									   << QString::fromUtf8(loopmidi_port) << "1"
									   << "15"
									   << "150"
									   << "200");
		std::this_thread::sleep_for(std::chrono::milliseconds(300));
		g_tbar_debug_log.store(false, std::memory_order_relaxed);

		int c5_started = g_obs_manual_started.load(std::memory_order_relaxed);
		int c5_finished = g_obs_manual_finished.load(std::memory_order_relaxed);
		int c5_releases = g_tbar_release_count.load(std::memory_order_relaxed);

		char c5_prev_name[128] = {0};
		char c5_prog_name[128] = {0};
		QMetaObject::invokeMethod(
			main_win,
			[&]() {
				obs_source_t *ps = obs_frontend_get_current_preview_scene();
				obs_source_t *cs = obs_frontend_get_current_scene();
				if (ps && obs_source_get_name(ps)) {
					std::snprintf(c5_prev_name, sizeof(c5_prev_name), "%s",
						      obs_source_get_name(ps));
				}
				if (cs && obs_source_get_name(cs)) {
					std::snprintf(c5_prog_name, sizeof(c5_prog_name), "%s",
						      obs_source_get_name(cs));
				}
				if (ps) {
					obs_source_release(ps);
				}
				if (cs) {
					obs_source_release(cs);
				}
			},
			Qt::BlockingQueuedConnection);

		bool c5_scenes_ok = (std::strcmp(c5_prev_name, exp_scene_a) == 0) &&
				    (std::strcmp(c5_prog_name, exp_scene_b) == 0);
		bool case5_ok = (rc_c5 == 0) && (c5_started == 3) && (c5_finished == 3) && (c5_releases == 3) &&
				c5_scenes_ok;

		obs_log(case5_ok ? LOG_INFO : LOG_ERROR,
			"LOOPMIDI CASE 5 (rapid throws <530ms) [%s]: started=%d finished=%d releases=%d preview='%s'(exp='%s') program='%s'(exp='%s')",
			case5_ok ? "PASS" : "FAIL", c5_started, c5_finished, c5_releases, c5_prev_name, exp_scene_a,
			c5_prog_name, exp_scene_b);

		/*
		 * Case (6): Slow crossfader sweep at 2 CC per 100 ms (0 -> 127 over 6350 ms = 50 ms/CC):
		 *   6A: Asserts no release before CC 124, manual transition time monotonic to 1.0, and exactly 1 release.
		 *   6B: GetSourceScreenshot of the Program output at CC 119 showing ~93% mix, NOT 100%.
		 */
		obs_source_t *saved_transition = nullptr;
		obs_source_t *program_tap = nullptr;
		char program_tap_uuid[128] = {0};
		char program_tr_id[64] = {0};
		QMetaObject::invokeMethod(
			main_win,
			[&]() {
				obs_source_t *cur_tr = obs_frontend_get_current_transition();
				if (cur_tr) {
					const char *cid = obs_source_get_id(cur_tr);
					if (!cid || std::strcmp(cid, "fade_transition") != 0) {
						struct obs_frontend_source_list tr_list = {};
						obs_frontend_get_transitions(&tr_list);
						for (size_t ti = 0; ti < tr_list.sources.num; ++ti) {
							obs_source_t *cand = tr_list.sources.array[ti];
							const char *tid = cand ? obs_source_get_id(cand) : nullptr;
							if (tid && std::strcmp(tid, "fade_transition") == 0) {
								saved_transition = obs_source_get_ref(cur_tr);
								obs_frontend_set_current_transition(cand);
								break;
							}
						}
						obs_frontend_source_list_free(&tr_list);
					}
					obs_source_release(cur_tr);
				}
				if (tbar_red_scene) {
					obs_frontend_set_current_scene(obs_scene_get_source(tbar_red_scene));
				}
			},
			Qt::BlockingQueuedConnection);
		/* Wait for Program auto-transition to tbar_red_scene (and its swapScenesMode callback) to finish */
		std::this_thread::sleep_for(std::chrono::milliseconds(650));

		QMetaObject::invokeMethod(
			main_win,
			[&]() {
				if (tbar_blue_scene) {
					obs_frontend_set_current_preview_scene(obs_scene_get_source(tbar_blue_scene));
				}
				program_tap = obs_source_create("mfd_selftest_program_tap",
								"__mfd_selftest_program_tap__", nullptr, nullptr);
				if (program_tap) {
					const char *uu = obs_source_get_uuid(program_tap);
					if (uu) {
						std::snprintf(program_tap_uuid, sizeof(program_tap_uuid), "%s", uu);
					}
				}
				obs_source_t *prog_tr = obs_get_output_source(0);
				if (prog_tr) {
					const char *tid = obs_source_get_id(prog_tr);
					if (tid) {
						std::snprintf(program_tr_id, sizeof(program_tr_id), "%s", tid);
					}
					obs_source_release(prog_tr);
				}
			},
			Qt::BlockingQueuedConnection);
		std::this_thread::sleep_for(std::chrono::milliseconds(100));

		mfd_tbar_reset_release_latch();
		mfd_takeover_init(&eng->fader_slots[0].takeover, 0.0f, MFD_DEFAULT_PICKUP_THRESHOLD, true);
		eng->fader_slots[0].takeover.state = MFD_TAKEOVER_ENGAGED;
		mfd_smoother_reset(&eng->fader_slots[0].smoother, 0.0f);
		eng->fader_slots[0].tbar_state = MFD_TBAR_IDLE;
		eng->fader_slots[0].last_applied_val = 0.0f;
		eng->fader_slots[0].tbar_midi_active = false;
		eng->last_midi_decoded.raw_value = 0U;
		g_last_pos_1024.store(0, std::memory_order_relaxed);
		g_obs_manual_started.store(0, std::memory_order_relaxed);
		g_obs_manual_finished.store(0, std::memory_order_relaxed);
		g_obs_manual_cancelled.store(0, std::memory_order_relaxed);
		g_tbar_release_count.store(0, std::memory_order_relaxed);
		g_tbar_first_release_raw_cc.store(-1, std::memory_order_relaxed);
		g_tbar_releases_before_cc124.store(0, std::memory_order_relaxed);
		g_tbar_writes_since_release.store(0, std::memory_order_relaxed);
		g_tbar_post_first_release_writes.store(0, std::memory_order_relaxed);

		obs_log(LOG_INFO,
			"LOOPMIDI CASE 6 (0->100 then 1 CC/300ms 100->127 with 2000ms pause at CC 116, tr='%s' tap_uuid='%s'): starting...",
			program_tr_id, program_tap_uuid);

		/* Bring fader from 0 to 100, then run 1 CC every 300 ms from 100 to 127 with a 2 s pause at CC 116 */
		int rc_c6_pre = QProcess::execute(midi_send_exe, QStringList()
									 << QString::fromUtf8(loopmidi_port) << "1"
									 << "15"
									 << "250"
									 << "0"
									 << "100");
		std::this_thread::sleep_for(std::chrono::milliseconds(60));

		QProcess slow_proc;
		slow_proc.start(midi_send_exe, QStringList()
						       << "--slow-pause" << QString::fromUtf8(loopmidi_port) << "1"
						       << "15"
						       << "100"
						       << "127"
						       << "300"
						       << "116"
						       << "2000");
		slow_proc.waitForStarted(1500);

		bool saw_sweep_start = false;
		bool has_tr_sample = false;
		float max_tr_time = 0.0f;
		int monotonic_violations = 0;
		int polled_releases_before_cc124 = 0;
		int releases_during_cc116_pause = 0;
		bool saw_cc116 = false;
		std::chrono::steady_clock::time_point cc116_first_tp{};
		std::chrono::steady_clock::time_point cc116_last_tp{};
		float tr_at_cc100 = -1.0f;
		float tr_at_cc116 = -1.0f;
		float tr_at_cc119 = -1.0f;
		float tr_at_cc123 = -1.0f;

		bool captured_cc119 = false;
		bool cc119_cap_ok = false;
		uint16_t cc119_cc_seen = 0;
		int cc119_rel_so_far = -1;
		int cc119_slider_pos = -1;
		float cc119_r = -1.0f;
		float cc119_g = -1.0f;
		float cc119_b = -1.0f;

		while (!slow_proc.waitForFinished(10)) {
			uint16_t cc_now = eng->last_midi_decoded.raw_value;
			if (cc_now >= 100U && cc_now <= 102U) {
				saw_sweep_start = true;
			}
			int rel_now = g_tbar_release_count.load(std::memory_order_relaxed);
			if (saw_sweep_start && cc_now < 124U && rel_now > polled_releases_before_cc124) {
				polled_releases_before_cc124 = rel_now;
			}
			if (cc_now == 116U) {
				const auto now_tp = std::chrono::steady_clock::now();
				if (!saw_cc116) {
					saw_cc116 = true;
					cc116_first_tp = now_tp;
				}
				cc116_last_tp = now_tp;
				if (rel_now > releases_during_cc116_pause) {
					releases_during_cc116_pause = rel_now;
				}
			}

			if (g_obs_manual_started.load(std::memory_order_relaxed) >= 1) {
				obs_source_t *tr_s = obs_get_output_source(0);
				if (tr_s) {
					float tr_t = obs_transition_get_time(tr_s);
					obs_source_release(tr_s);
					if (has_tr_sample && (tr_t + 1e-4f < max_tr_time)) {
						++monotonic_violations;
					}
					has_tr_sample = true;
					if (tr_t > max_tr_time) {
						max_tr_time = tr_t;
					}
					if (cc_now == 100U && tr_at_cc100 < 0.0f) {
						tr_at_cc100 = tr_t;
					}
					if (cc_now == 116U) {
						tr_at_cc116 = tr_t;
					}
					if (cc_now == 123U && tr_at_cc123 < 0.0f) {
						tr_at_cc123 = tr_t;
					}
				}
			}

			if (saw_sweep_start && !captured_cc119 && cc_now >= 119U) {
				captured_cc119 = true;
				cc119_cc_seen = cc_now;
				std::this_thread::sleep_for(std::chrono::milliseconds(40));
				cc119_rel_so_far = g_tbar_release_count.load(std::memory_order_relaxed);
				obs_source_t *tr_119 = obs_get_output_source(0);
				if (tr_119) {
					tr_at_cc119 = obs_transition_get_time(tr_119);
					obs_source_release(tr_119);
				}
				QMetaObject::invokeMethod(
					main_win,
					[&]() { cc119_slider_pos = g_tbar_slider ? g_tbar_slider->value() : -1; },
					Qt::BlockingQueuedConnection);
				cc119_cap_ok = capture_source_screenshot_by_uuid_srgb(program_tap_uuid, &cc119_r,
										      &cc119_g, &cc119_b);
			}
		}

		int rc_c6 = (rc_c6_pre == 0 && slow_proc.exitStatus() == QProcess::NormalExit) ? slow_proc.exitCode()
											       : -1;
		std::this_thread::sleep_for(std::chrono::milliseconds(350));

		const long long cc116_dwell_ms =
			saw_cc116
				? std::chrono::duration_cast<std::chrono::milliseconds>(cc116_last_tp - cc116_first_tp)
					  .count()
				: 0LL;

		float final_tr_time = -1.0f;
		obs_source_t *tr_end = obs_get_output_source(0);
		if (tr_end) {
			final_tr_time = obs_transition_get_time(tr_end);
			obs_source_release(tr_end);
			if (has_tr_sample && (final_tr_time + 1e-4f < max_tr_time)) {
				++monotonic_violations;
			}
			if (final_tr_time > max_tr_time) {
				max_tr_time = final_tr_time;
			}
		}

		float end_srgb_r = -1.0f;
		float end_srgb_g = -1.0f;
		float end_srgb_b = -1.0f;
		bool end_cap_ok =
			capture_source_screenshot_by_uuid_srgb(program_tap_uuid, &end_srgb_r, &end_srgb_g, &end_srgb_b);

		QMetaObject::invokeMethod(
			main_win,
			[&]() {
				if (program_tap) {
					obs_source_remove(program_tap);
					obs_source_release(program_tap);
					program_tap = nullptr;
				}
				if (saved_transition) {
					obs_frontend_set_current_transition(saved_transition);
					obs_source_release(saved_transition);
					saved_transition = nullptr;
				}
			},
			Qt::BlockingQueuedConnection);

		int c6_started = g_obs_manual_started.load(std::memory_order_relaxed);
		int c6_finished = g_obs_manual_finished.load(std::memory_order_relaxed);
		int c6_cancelled = g_obs_manual_cancelled.load(std::memory_order_relaxed);
		int c6_releases = g_tbar_release_count.load(std::memory_order_relaxed);
		int first_rel_cc = g_tbar_first_release_raw_cc.load(std::memory_order_relaxed);
		int rel_before_124 = std::max(g_tbar_releases_before_cc124.load(std::memory_order_relaxed),
					      polled_releases_before_cc124);
		bool c6_parked = (eng->fader_slots[0].takeover.state == MFD_TAKEOVER_PARKED);

		bool case6a_slow_sweep_ok =
			(rc_c6 == 0) && saw_cc116 && (cc116_dwell_ms >= 1800LL) && (releases_during_cc116_pause == 0) &&
			(rel_before_124 == 0) && (first_rel_cc == 124) && (c6_releases == 1) && (c6_started == 1) &&
			(c6_finished == 1) && (c6_cancelled == 0) && has_tr_sample && (monotonic_violations == 0) &&
			(std::fabs(final_tr_time - 1.0f) <= 1e-4f) && c6_parked;

		bool case6b_cc119_mix_ok = cc119_cap_ok && (cc119_b >= 0.89f && cc119_b <= 0.965f) &&
					   (cc119_r >= 0.035f && cc119_r <= 0.11f) && (cc119_g <= 0.02f) &&
					   end_cap_ok && (end_srgb_b >= 0.99f && end_srgb_r <= 0.01f);

		bool case6_ok = case6a_slow_sweep_ok && case6b_cc119_mix_ok;

		obs_log(case6a_slow_sweep_ok ? LOG_INFO : LOG_ERROR,
			"LOOPMIDI CASE 6A (1 CC/300ms 100->127 + 2s pause at CC 116) [%s]: rc=%d cc116_dwell_ms=%lld releases_at_cc116=%d releases_before_cc124=%d first_release_cc=%d total_releases=%d started=%d finished=%d cancelled=%d monotonic_violations=%d tr_time(cc100=%.4f cc116=%.4f cc119=%.4f cc123=%.4f final=%.4f) parked=%d",
			case6a_slow_sweep_ok ? "PASS" : "FAIL", rc_c6, cc116_dwell_ms, releases_during_cc116_pause,
			rel_before_124, first_rel_cc, c6_releases, c6_started, c6_finished, c6_cancelled,
			monotonic_violations, tr_at_cc100, tr_at_cc116, tr_at_cc119, tr_at_cc123, final_tr_time,
			(int)c6_parked);

		obs_log(case6b_cc119_mix_ok ? LOG_INFO : LOG_ERROR,
			"LOOPMIDI CASE 6B (Program GetSourceScreenshot at CC %u) [%s]: cap_ok=%d srgb_rgb=(%.4f, %.4f, %.4f) mix_pct=%.1f%% (expected ~93.7%%, NOT 100%%) tr_time_at_cc119=%.4f slider_pos=%d releases_at_cc119=%d end_srgb_rgb=(%.4f, %.4f, %.4f)",
			(unsigned)cc119_cc_seen, case6b_cc119_mix_ok ? "PASS" : "FAIL", (int)cc119_cap_ok, cc119_r,
			cc119_g, cc119_b, cc119_b * 100.0f, tr_at_cc119, cc119_slider_pos, cc119_rel_so_far, end_srgb_r,
			end_srgb_g, end_srgb_b);

		/*
		 * Case (7): Throwing -> Idle (Aborted throw at origin rail) & follow-up throw (R-TBAR-SM):
		 *   7A: loopMIDI 0 -> 60 -> 0 asserts 1 release, Program unchanged, Transition button enabled again, 0 further writes.
		 *   7B: loopMIDI 0 -> 60 -> pause -> 127 asserts 1 release at the rail only.
		 */
		set_studio_scenes_and_wait(test_scene, test_scene_b);

		mfd_tbar_reset_release_latch();
		mfd_takeover_init(&eng->fader_slots[0].takeover, 0.0f, MFD_DEFAULT_PICKUP_THRESHOLD, true);
		eng->fader_slots[0].takeover.state = MFD_TAKEOVER_ENGAGED;
		mfd_smoother_reset(&eng->fader_slots[0].smoother, 0.0f);
		eng->fader_slots[0].tbar_state = MFD_TBAR_IDLE;
		eng->fader_slots[0].last_applied_val = 0.0f;
		eng->fader_slots[0].tbar_midi_active = false;
		g_last_pos_1024.store(0, std::memory_order_relaxed);
		g_obs_manual_started.store(0, std::memory_order_relaxed);
		g_obs_manual_finished.store(0, std::memory_order_relaxed);
		g_obs_manual_cancelled.store(0, std::memory_order_relaxed);
		g_tbar_set_count.store(0, std::memory_order_relaxed);
		g_tbar_release_count.store(0, std::memory_order_relaxed);
		g_tbar_writes_since_release.store(0, std::memory_order_relaxed);

		bool btn_tr_found = false;
		bool btn_tr_enabled_before = false;
		bool btn_tr_enabled_mid = true;
		bool btn_tr_enabled_after = false;
		QMetaObject::invokeMethod(
			main_win,
			[&]() {
				QPushButton *btn_tr = locate_studio_transition_button_on_ui_thread();
				btn_tr_found = (btn_tr != nullptr);
				btn_tr_enabled_before = btn_tr ? btn_tr->isEnabled() : false;
			},
			Qt::BlockingQueuedConnection);

		int rc_c7a_up = QProcess::execute(midi_send_exe, QStringList()
									 << QString::fromUtf8(loopmidi_port) << "1"
									 << "15"
									 << "250"
									 << "0"
									 << "60");
		std::this_thread::sleep_for(std::chrono::milliseconds(100));

		int c7a_mid_releases = g_tbar_release_count.load(std::memory_order_relaxed);
		enum mfd_tbar_state c7a_mid_state = eng->fader_slots[0].tbar_state;
		QMetaObject::invokeMethod(
			main_win,
			[&]() {
				QPushButton *btn_tr = locate_studio_transition_button_on_ui_thread();
				btn_tr_enabled_mid = btn_tr ? btn_tr->isEnabled() : true;
			},
			Qt::BlockingQueuedConnection);

		int rc_c7a_down = QProcess::execute(midi_send_exe, QStringList()
									   << QString::fromUtf8(loopmidi_port) << "1"
									   << "15"
									   << "250"
									   << "60"
									   << "0");
		std::this_thread::sleep_for(std::chrono::milliseconds(150));
		int c7a_writes_at_abort = g_tbar_set_count.load(std::memory_order_relaxed);

		int rc_c7a_idle_jitter = QProcess::execute(
			midi_send_exe, QStringList() << "--jitter" << QString::fromUtf8(loopmidi_port) << "1"
						     << "15"
						     << "30"
						     << "4"
						     << "2"
						     << "0");
		std::this_thread::sleep_for(std::chrono::milliseconds(100));

		int c7a_started = g_obs_manual_started.load(std::memory_order_relaxed);
		int c7a_finished = g_obs_manual_finished.load(std::memory_order_relaxed);
		int c7a_cancelled = g_obs_manual_cancelled.load(std::memory_order_relaxed);
		int c7a_releases = g_tbar_release_count.load(std::memory_order_relaxed);
		int c7a_further_writes = g_tbar_writes_since_release.load(std::memory_order_relaxed) +
					 (g_tbar_set_count.load(std::memory_order_relaxed) - c7a_writes_at_abort);
		enum mfd_tbar_state c7a_end_state = eng->fader_slots[0].tbar_state;
		bool c7a_pol_rev = eng->fader_slots[0].takeover.polarity_reversed;
		bool c7a_parked = (eng->fader_slots[0].takeover.state == MFD_TAKEOVER_PARKED);

		char c7a_prev_name[128] = {0};
		char c7a_prog_name[128] = {0};
		QMetaObject::invokeMethod(
			main_win,
			[&]() {
				QPushButton *btn_tr = locate_studio_transition_button_on_ui_thread();
				btn_tr_enabled_after = btn_tr ? btn_tr->isEnabled() : false;
				obs_source_t *ps = obs_frontend_get_current_preview_scene();
				obs_source_t *cs = obs_frontend_get_current_scene();
				if (ps && obs_source_get_name(ps)) {
					std::snprintf(c7a_prev_name, sizeof(c7a_prev_name), "%s",
						      obs_source_get_name(ps));
				}
				if (cs && obs_source_get_name(cs)) {
					std::snprintf(c7a_prog_name, sizeof(c7a_prog_name), "%s",
						      obs_source_get_name(cs));
				}
				if (ps) {
					obs_source_release(ps);
				}
				if (cs) {
					obs_source_release(cs);
				}
			},
			Qt::BlockingQueuedConnection);

		bool c7a_prog_unchanged = (std::strcmp(c7a_prog_name, exp_scene_a) == 0) &&
					  (std::strcmp(c7a_prev_name, exp_scene_b) == 0);
		bool c7a_btn_ok = btn_tr_found && btn_tr_enabled_before && !btn_tr_enabled_mid && btn_tr_enabled_after;
		bool case7a_ok = (rc_c7a_up == 0) && (rc_c7a_down == 0) && (rc_c7a_idle_jitter == 0) &&
				 (c7a_mid_releases == 0) && (c7a_mid_state == MFD_TBAR_THROWING) &&
				 (c7a_started == 1) && (c7a_cancelled == 1) && (c7a_finished == 0) &&
				 (c7a_releases == 1) && c7a_prog_unchanged && c7a_btn_ok &&
				 (c7a_end_state == MFD_TBAR_IDLE) && !c7a_pol_rev && !c7a_parked &&
				 (c7a_further_writes == 0);

		obs_log(case7a_ok ? LOG_INFO : LOG_ERROR,
			"LOOPMIDI CASE 7A (0->60->0 aborted throw) [%s]: started=%d cancelled=%d finished=%d releases=%d(mid=%d) further_writes=%d program='%s'(unchanged_exp='%s') preview='%s' transition_btn(found=%d before=%d mid=%d after=%d) end_state=%d pol_rev=%d parked=%d",
			case7a_ok ? "PASS" : "FAIL", c7a_started, c7a_cancelled, c7a_finished, c7a_releases,
			c7a_mid_releases, c7a_further_writes, c7a_prog_name, exp_scene_a, c7a_prev_name,
			(int)btn_tr_found, (int)btn_tr_enabled_before, (int)btn_tr_enabled_mid,
			(int)btn_tr_enabled_after, (int)c7a_end_state, (int)c7a_pol_rev, (int)c7a_parked);

		/* Case 7B: Immediately after abort (no slot reset), 0 -> 60 -> pause (800 ms) -> 127 */
		g_obs_manual_started.store(0, std::memory_order_relaxed);
		g_obs_manual_finished.store(0, std::memory_order_relaxed);
		g_obs_manual_cancelled.store(0, std::memory_order_relaxed);
		g_tbar_release_count.store(0, std::memory_order_relaxed);
		g_tbar_first_release_raw_cc.store(-1, std::memory_order_relaxed);
		g_tbar_releases_before_cc124.store(0, std::memory_order_relaxed);
		g_tbar_writes_since_release.store(0, std::memory_order_relaxed);

		int rc_c7b_p1 = QProcess::execute(midi_send_exe, QStringList()
									 << QString::fromUtf8(loopmidi_port) << "1"
									 << "15"
									 << "250"
									 << "0"
									 << "60");
		/* Pause 800 ms at CC 60 */
		std::this_thread::sleep_for(std::chrono::milliseconds(800));
		int c7b_pause_releases = g_tbar_release_count.load(std::memory_order_relaxed);
		enum mfd_tbar_state c7b_pause_state = eng->fader_slots[0].tbar_state;

		int rc_c7b_p2 = QProcess::execute(midi_send_exe, QStringList()
									 << QString::fromUtf8(loopmidi_port) << "1"
									 << "15"
									 << "350"
									 << "60"
									 << "127");
		std::this_thread::sleep_for(std::chrono::milliseconds(550));

		int c7b_started = g_obs_manual_started.load(std::memory_order_relaxed);
		int c7b_finished = g_obs_manual_finished.load(std::memory_order_relaxed);
		int c7b_cancelled = g_obs_manual_cancelled.load(std::memory_order_relaxed);
		int c7b_releases = g_tbar_release_count.load(std::memory_order_relaxed);
		int c7b_first_rel_cc = g_tbar_first_release_raw_cc.load(std::memory_order_relaxed);
		int c7b_rel_before_124 = g_tbar_releases_before_cc124.load(std::memory_order_relaxed);
		int c7b_post_writes = g_tbar_writes_since_release.load(std::memory_order_relaxed);
		bool c7b_parked = (eng->fader_slots[0].takeover.state == MFD_TAKEOVER_PARKED);

		char c7b_prev_name[128] = {0};
		char c7b_prog_name[128] = {0};
		QMetaObject::invokeMethod(
			main_win,
			[&]() {
				obs_source_t *ps = obs_frontend_get_current_preview_scene();
				obs_source_t *cs = obs_frontend_get_current_scene();
				if (ps && obs_source_get_name(ps)) {
					std::snprintf(c7b_prev_name, sizeof(c7b_prev_name), "%s",
						      obs_source_get_name(ps));
				}
				if (cs && obs_source_get_name(cs)) {
					std::snprintf(c7b_prog_name, sizeof(c7b_prog_name), "%s",
						      obs_source_get_name(cs));
				}
				if (ps) {
					obs_source_release(ps);
				}
				if (cs) {
					obs_source_release(cs);
				}
			},
			Qt::BlockingQueuedConnection);

		bool c7b_scenes_swapped = (std::strcmp(c7b_prev_name, exp_scene_a) == 0) &&
					  (std::strcmp(c7b_prog_name, exp_scene_b) == 0);
		bool case7b_ok = (rc_c7b_p1 == 0) && (rc_c7b_p2 == 0) && (c7b_pause_releases == 0) &&
				 (c7b_pause_state == MFD_TBAR_THROWING) && (c7b_started == 1) && (c7b_finished == 1) &&
				 (c7b_cancelled == 0) && (c7b_releases == 1) && (c7b_rel_before_124 == 0) &&
				 (c7b_first_rel_cc == 124) && (c7b_post_writes == 0) && c7b_parked &&
				 c7b_scenes_swapped;

		obs_log(case7b_ok ? LOG_INFO : LOG_ERROR,
			"LOOPMIDI CASE 7B (0->60->pause 800ms->127) [%s]: pause_releases=%d pause_state=%d started=%d finished=%d cancelled=%d releases=%d first_release_cc=%d releases_before_124=%d post_writes=%d parked=%d program='%s'(exp='%s') preview='%s'(exp='%s')",
			case7b_ok ? "PASS" : "FAIL", c7b_pause_releases, (int)c7b_pause_state, c7b_started,
			c7b_finished, c7b_cancelled, c7b_releases, c7b_first_rel_cc, c7b_rel_before_124,
			c7b_post_writes, (int)c7b_parked, c7b_prog_name, exp_scene_b, c7b_prev_name, exp_scene_a);

		bool case7_ok = case7a_ok && case7b_ok;

		/*
		 * Case (8): 0.1.3 Bottom-Rail 6% Hysteresis Start Band (R-TBAR-SM / D-8):
		 *   8A: CC 3,4,3,4,3,4 five times (30 steps) from Idle -> zero transitions (0 starts, 0 cancels, 0 finishes, 0 writes).
		 *   8B: CC 0 -> 8 -> 3 -> one start (at CC 8 > 6%) and one cancel (at CC 3 <= 3%), Program unchanged, Transition button enabled.
		 */
		set_studio_scenes_and_wait(test_scene, test_scene_b);

		mfd_tbar_reset_release_latch();
		mfd_takeover_init(&eng->fader_slots[0].takeover, 0.0f, MFD_DEFAULT_PICKUP_THRESHOLD, true);
		eng->fader_slots[0].takeover.state = MFD_TAKEOVER_ENGAGED;
		mfd_smoother_reset(&eng->fader_slots[0].smoother, 0.0f);
		eng->fader_slots[0].tbar_state = MFD_TBAR_IDLE;
		eng->fader_slots[0].tbar_from_idle_band = false;
		eng->fader_slots[0].last_applied_val = 0.0f;
		eng->fader_slots[0].tbar_midi_active = false;
		g_last_pos_1024.store(0, std::memory_order_relaxed);
		g_obs_manual_started.store(0, std::memory_order_relaxed);
		g_obs_manual_finished.store(0, std::memory_order_relaxed);
		g_obs_manual_cancelled.store(0, std::memory_order_relaxed);
		g_tbar_set_count.store(0, std::memory_order_relaxed);
		g_tbar_release_count.store(0, std::memory_order_relaxed);
		g_tbar_writes_since_release.store(0, std::memory_order_relaxed);

		int rc_c8a = QProcess::execute(
			midi_send_exe, QStringList() << "--seq" << QString::fromUtf8(loopmidi_port) << "1"
						     << "15"
						     << "20"
						     << "3,4,3,4,3,4,3,4,3,4,3,4,3,4,3,4,3,4,3,4,3,4,3,4,3,4,3,4,3,4");
		std::this_thread::sleep_for(std::chrono::milliseconds(150));

		int c8a_started = g_obs_manual_started.load(std::memory_order_relaxed);
		int c8a_finished = g_obs_manual_finished.load(std::memory_order_relaxed);
		int c8a_cancelled = g_obs_manual_cancelled.load(std::memory_order_relaxed);
		int c8a_writes = g_tbar_set_count.load(std::memory_order_relaxed);
		int c8a_releases = g_tbar_release_count.load(std::memory_order_relaxed);
		enum mfd_tbar_state c8a_state = eng->fader_slots[0].tbar_state;
		bool case8a_ok = (rc_c8a == 0) && (c8a_started == 0) && (c8a_finished == 0) && (c8a_cancelled == 0) &&
				 (c8a_writes == 0) && (c8a_releases == 0) && (c8a_state == MFD_TBAR_IDLE);

		obs_log(case8a_ok ? LOG_INFO : LOG_ERROR,
			"LOOPMIDI CASE 8A (CC 3,4,3,4,3,4 x5 bottom-rail hysteresis) [%s]: started=%d finished=%d cancelled=%d writes=%d releases=%d state=%d",
			case8a_ok ? "PASS" : "FAIL", c8a_started, c8a_finished, c8a_cancelled, c8a_writes, c8a_releases,
			(int)c8a_state);

		int rc_c8b = QProcess::execute(midi_send_exe, QStringList() << "--seq"
									    << QString::fromUtf8(loopmidi_port) << "1"
									    << "15"
									    << "40"
									    << "0,8,3");
		std::this_thread::sleep_for(std::chrono::milliseconds(150));

		int c8b_started = g_obs_manual_started.load(std::memory_order_relaxed);
		int c8b_finished = g_obs_manual_finished.load(std::memory_order_relaxed);
		int c8b_cancelled = g_obs_manual_cancelled.load(std::memory_order_relaxed);
		int c8b_releases = g_tbar_release_count.load(std::memory_order_relaxed);
		enum mfd_tbar_state c8b_state = eng->fader_slots[0].tbar_state;
		bool c8b_btn_after = false;
		char c8b_prev_name[128] = {0};
		char c8b_prog_name[128] = {0};
		QMetaObject::invokeMethod(
			main_win,
			[&]() {
				QPushButton *btn_tr = locate_studio_transition_button_on_ui_thread();
				c8b_btn_after = btn_tr ? btn_tr->isEnabled() : false;
				obs_source_t *ps = obs_frontend_get_current_preview_scene();
				obs_source_t *cs = obs_frontend_get_current_scene();
				if (ps && obs_source_get_name(ps)) {
					std::snprintf(c8b_prev_name, sizeof(c8b_prev_name), "%s",
						      obs_source_get_name(ps));
				}
				if (cs && obs_source_get_name(cs)) {
					std::snprintf(c8b_prog_name, sizeof(c8b_prog_name), "%s",
						      obs_source_get_name(cs));
				}
				if (ps) {
					obs_source_release(ps);
				}
				if (cs) {
					obs_source_release(cs);
				}
			},
			Qt::BlockingQueuedConnection);

		bool c8b_prog_unchanged = (std::strcmp(c8b_prog_name, exp_scene_a) == 0) &&
					  (std::strcmp(c8b_prev_name, exp_scene_b) == 0);
		bool case8b_ok = (rc_c8b == 0) && (c8b_started == 1) && (c8b_cancelled == 1) && (c8b_finished == 0) &&
				 (c8b_releases == 1) && (c8b_state == MFD_TBAR_IDLE) && c8b_btn_after &&
				 c8b_prog_unchanged;

		obs_log(case8b_ok ? LOG_INFO : LOG_ERROR,
			"LOOPMIDI CASE 8B (CC 0->8->3 one start & one cancel) [%s]: started=%d cancelled=%d finished=%d releases=%d state=%d btn_enabled=%d program='%s'(exp='%s') preview='%s'(exp='%s')",
			case8b_ok ? "PASS" : "FAIL", c8b_started, c8b_cancelled, c8b_finished, c8b_releases,
			(int)c8b_state, (int)c8b_btn_after, c8b_prog_name, exp_scene_a, c8b_prev_name, exp_scene_b);

		bool case8_ok = case8a_ok && case8b_ok;

		loopmidi_pass = connected && pre_waiting_ok && pre_slider_held && saw_waiting_before_pickup &&
				saw_engaged_after_pickup && smoothing_ok && sweep_completed_throw &&
				(final_cc == 127) && case2_ok && case3_ok && case4_ok && case5_ok && case6_ok &&
				case7_ok && case8_ok;

		obs_log(loopmidi_pass ? LOG_INFO : LOG_ERROR,
			"LOOPMIDI SWEEP CHECK [%s]: port='%s' connected=%d pre_waiting=%d(slider=%d) saw_waiting_pre_pickup=%d saw_engaged_post_pickup=%d mid_samples=%d max_smoothing_lag=%.4f final_cc=%u throw_completed=%d case2_ok=%d case3_ok=%d case4_ok=%d case5_ok=%d case6_ok=%d case7_ok=%d case8_ok=%d",
			loopmidi_pass ? "PASS" : "FAIL", loopmidi_port, (int)connected, (int)pre_waiting_ok, slider_pre,
			(int)saw_waiting_before_pickup, (int)saw_engaged_after_pickup, mid_sweep_samples,
			max_smoothing_lag, (unsigned)final_cc, (int)sweep_completed_throw, (int)case2_ok, (int)case3_ok,
			(int)case4_ok, (int)case5_ok, (int)case6_ok, (int)case7_ok, (int)case8_ok);

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

	/* Clean up test scenes/sources and always leave Studio Mode OFF before closing OBS */
	bool studio_off_after = false;
	QMetaObject::invokeMethod(
		main_win,
		[&]() {
			if (obs_frontend_preview_program_mode_active()) {
				obs_frontend_set_preview_program_mode(false);
				refresh_studio_mode_state_on_ui_thread();
			}
			if (prev_scene) {
				obs_frontend_set_current_scene(prev_scene);
				obs_source_release(prev_scene);
			}
			studio_off_after = !obs_frontend_preview_program_mode_active();
		},
		Qt::BlockingQueuedConnection);

	std::this_thread::sleep_for(std::chrono::milliseconds(600));

	QMetaObject::invokeMethod(
		main_win,
		[&]() {
			auto detach_opacity_filter = [](obs_source_t *src) {
				if (!src) {
					return;
				}
				obs_source_t *flt =
					obs_source_get_filter_by_name(src, MFD_INTERNAL_OPACITY_FILTER_NAME);
				if (flt) {
					obs_source_filter_remove(src, flt);
					obs_source_release(flt);
				}
			};
			if (src_a) {
				detach_opacity_filter(src_a);
				obs_source_remove(src_a);
				obs_source_release(src_a);
			}
			if (src_b) {
				detach_opacity_filter(src_b);
				obs_source_remove(src_b);
				obs_source_release(src_b);
			}
			if (tbar_src_red) {
				obs_source_remove(tbar_src_red);
				obs_source_release(tbar_src_red);
			}
			if (tbar_src_blue) {
				obs_source_remove(tbar_src_blue);
				obs_source_release(tbar_src_blue);
			}
			if (tbar_blue_scene) {
				obs_source_remove(obs_scene_get_source(tbar_blue_scene));
				obs_scene_release(tbar_blue_scene);
			}
			if (tbar_red_scene) {
				obs_source_remove(obs_scene_get_source(tbar_red_scene));
				obs_scene_release(tbar_red_scene);
			}
			if (test_scene_b) {
				obs_source_remove(obs_scene_get_source(test_scene_b));
				obs_scene_release(test_scene_b);
			}
			if (test_scene) {
				obs_source_remove(obs_scene_get_source(test_scene));
				obs_scene_release(test_scene);
			}
		},
		Qt::BlockingQueuedConnection);

	base_set_log_handler(g_orig_log_handler, g_orig_log_param);

	obs_log(LOG_INFO,
		"SELF-TEST RESULT: build_ok=%d slot_avail=%d filter_a=%.2f->%.2f filter_b=%.2f->%.2f screenshot_all_pass=%d loopmidi_tested=%d loopmidi_pass=%d slider_mid=%d tbar_mid_ok=%d tbar_end_ok=%d tbar_completed=%d studio_off=%d bridge_ok=%d/%.2f sig_val=%.2f(%s) vs_received_fader_2=%.2f",
		(int)ws_build_ok, (int)slot_avail, read_a_mid, read_a_end, read_b_mid, read_b_end,
		(int)screenshot_all_pass, (int)loopmidi_tested, (int)loopmidi_pass, slider_mid, (int)mid_ok,
		(int)end_ok, (int)completed, (int)studio_off_after, (int)(bridge_set_ok && bridge_get_ok),
		bridge_get_val, sig_cap.value, sig_cap.slot, vs_fader2_val);
}

static std::thread g_selftest_thread;
static std::atomic<bool> g_selftest_running{false};

static void run_live_selftest_entry(void)
{
	run_live_selftest_worker();
	g_selftest_running.store(false, std::memory_order_release);
}

static void run_live_selftest_if_requested(void)
{
	if (!qEnvironmentVariableIsSet("MFD_SELF_TEST")) {
		return;
	}
	if (g_selftest_running.exchange(true, std::memory_order_acq_rel)) {
		return;
	}
	if (g_selftest_thread.joinable()) {
		g_selftest_thread.join();
	}
	g_selftest_thread = std::thread(run_live_selftest_entry);
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
		g_tbar_ui_throw_active = false;
		g_tbar_awaiting_transition_stop = false;
		g_pending_deferred_tbar.active = false;
		refresh_studio_mode_state_on_ui_thread();
		break;
	case OBS_FRONTEND_EVENT_TRANSITION_STOPPED: {
		const bool external_reset = g_tbar_ui_throw_active && !g_tbar_awaiting_transition_stop;
		g_tbar_ui_throw_active = false;
		g_tbar_awaiting_transition_stop = false;
		refresh_studio_mode_state_on_ui_thread();
		if (external_reset) {
			mfd_engine_on_external_tbar_reset(mfd_engine_get());
		}
		if (g_pending_deferred_tbar.active) {
			if (!g_main_win) {
				g_main_win = static_cast<QWidget *>(obs_frontend_get_main_window());
			}
			QWidget *main_win = g_main_win.data();
			if (main_win) {
				/*
				 * Queue on the Qt event loop (zero timers) so OBSBasic::TransitionStopped()
				 * finishes clearing swapScene = nullptr (OBSBasic_Transitions.cpp:271)
				 * before the next throw starts.
				 */
				QMetaObject::invokeMethod(
					main_win, []() { flush_deferred_tbar_on_ui_thread(); }, Qt::QueuedConnection);
			}
		}
		break;
	}
	default:
		break;
	}
}

bool mfd_tbar_is_studio_mode_active(void)
{
	QWidget *main_win = g_main_win.data();
	if (main_win && QThread::currentThread() == main_win->thread()) {
		bool active = obs_frontend_preview_program_mode_active();
		g_studio_mode_active.store(active, std::memory_order_relaxed);
		if (active && g_tbar_slider.isNull()) {
			refresh_studio_mode_state_on_ui_thread();
		}
	}
	return g_studio_mode_active.load(std::memory_order_relaxed);
}

bool mfd_tbar_driver_apply(enum mfd_tbar_state state, float t_norm, bool force_complete, bool force_abort,
			   bool in_target_rail_zone, bool in_origin_rail_zone, bool in_start_deadband,
			   enum mfd_tbar_state *out_next_state, bool *out_completed_throw, bool *out_aborted_throw,
			   char *out_err, size_t err_size)
{
	if (out_next_state) {
		*out_next_state = state;
	}
	if (out_completed_throw) {
		*out_completed_throw = false;
	}
	if (out_aborted_throw) {
		*out_aborted_throw = false;
	}
	if (out_err && err_size > 0U) {
		out_err[0] = '\0';
	}

	bool studio_active = mfd_tbar_is_studio_mode_active();
	bool slider_found = g_slider_tbar_found.load(std::memory_order_relaxed);
	bool latched = g_tbar_release_latched.load(std::memory_order_acquire);
	uint64_t epoch = g_tbar_throw_epoch.load(std::memory_order_acquire);
	int last_pos = g_last_pos_1024.load(std::memory_order_relaxed);

	struct mfd_tbar_release_input in;
	std::memset(&in, 0, sizeof(in));
	in.studio_mode_active = studio_active;
	in.slider_tbar_found = slider_found;
	in.state = state;
	in.release_latched = latched;
	in.force_complete = force_complete;
	in.force_abort = force_abort;
	in.in_target_rail_zone = in_target_rail_zone;
	in.in_origin_rail_zone = in_origin_rail_zone;
	in.in_start_deadband = in_start_deadband;
	in.t_norm = t_norm;
	in.last_pos_1024 = last_pos;

	struct mfd_tbar_action act;
	mfd_tbar_compute_action_full(&in, &act);
	if (out_next_state) {
		*out_next_state = act.next_state;
	}

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
		if (g_tbar_release_latched.exchange(true, std::memory_order_acq_rel)) {
			/* Per-transition release latch already tripped; block both slider write and release. */
			return true;
		}
		if (act.completed_throw) {
			struct mfd_engine *eng_rel = mfd_engine_get();
			if (eng_rel && eng_rel->has_last_midi) {
				int raw_cc = static_cast<int>(eng_rel->last_midi_decoded.raw_value);
				int expected_unset = -1;
				g_tbar_first_release_raw_cc.compare_exchange_strong(expected_unset, raw_cc,
										    std::memory_order_relaxed);
				bool pol_rev = eng_rel->fader_slots[0].takeover.polarity_reversed;
				float x_cal = eng_rel->fader_slots[0].last_raw_norm;
				if (!mfd_tbar_is_in_target_rail_zone(x_cal, pol_rev)) {
					g_tbar_releases_before_cc124.fetch_add(1, std::memory_order_relaxed);
				}
			}
		}
		g_last_pos_1024.store(0, std::memory_order_relaxed);
		if (out_completed_throw) {
			*out_completed_throw = act.completed_throw;
		}
		if (out_aborted_throw) {
			*out_aborted_throw = act.aborted_throw;
		}
	} else {
		if (g_tbar_release_latched.load(std::memory_order_acquire)) {
			return true;
		}
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
	if (act.completed_throw) {
		g_tbar_awaiting_unpark.store(true, std::memory_order_relaxed);
	}

	QWidget *main_win = g_main_win.data();
	if (!main_win) {
		return true;
	}

	const int pos = act.target_pos_1024;
	const bool do_set = act.call_set_tbar;
	const bool do_rel = act.call_release_tbar;

	if (QThread::currentThread() == main_win->thread()) {
		execute_tbar_on_ui_thread(pos, do_set, do_rel, epoch);
	} else {
		QMetaObject::invokeMethod(
			main_win,
			[pos, do_set, do_rel, epoch]() { execute_tbar_on_ui_thread(pos, do_set, do_rel, epoch); },
			Qt::QueuedConnection);
	}

	return true;
}

void mfd_tbar_init(void)
{
	g_main_win = static_cast<QWidget *>(obs_frontend_get_main_window());
	g_studio_mode_active.store(false, std::memory_order_relaxed);
	g_slider_tbar_found.store(false, std::memory_order_relaxed);
	g_logged_missing_error.store(false, std::memory_order_relaxed);
	g_last_pos_1024.store(0, std::memory_order_relaxed);
	g_tbar_release_latched.store(false, std::memory_order_relaxed);
	g_tbar_throw_epoch.store(1ULL, std::memory_order_relaxed);
	g_tbar_ui_last_released_epoch = 0ULL;
	g_tbar_ui_throw_active = false;
	g_tbar_awaiting_transition_stop = false;
	g_pending_deferred_tbar.active = false;

#ifdef MFD_ENABLE_SELF_TEST
	register_selftest_program_tap_once();
#endif
	mfd_engine_set_tbar_driver(mfd_tbar_driver_apply);
	mfd_engine_set_tbar_unlatch_fn(mfd_tbar_reset_release_latch);
	obs_frontend_add_event_callback(on_tbar_frontend_event, nullptr);
}

void mfd_tbar_shutdown(void)
{
#ifdef MFD_ENABLE_SELF_TEST
	while (g_selftest_running.load(std::memory_order_acquire)) {
		QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	if (g_selftest_thread.joinable()) {
		g_selftest_thread.join();
	}
#endif
	obs_frontend_remove_event_callback(on_tbar_frontend_event, nullptr);
	mfd_engine_set_tbar_unlatch_fn(nullptr);
	mfd_engine_set_tbar_driver(nullptr);
	g_pending_deferred_tbar.active = false;
	g_tbar_slider.clear();
	g_main_win.clear();
}
