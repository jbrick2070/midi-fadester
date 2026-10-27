/*
 * Midi Fadester
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "obs/mfd-tbar.h"
#include "obs/mfd-engine.h"
#include "core/tbar-logic.h"

#include <obs-module.h>
#include <obs-frontend-api.h>
#include "plugin-support.h"

#include <QApplication>
#include <QMetaObject>
#include <QPointer>
#include <QSlider>
#include <QThread>
#include <QVariant>
#include <QWidget>

#include <atomic>
#include <cstdio>

static QPointer<QSlider> g_tbar_slider;
static std::atomic<bool> g_studio_mode_active{false};
static std::atomic<bool> g_slider_tbar_found{false};
static std::atomic<bool> g_logged_missing_error{false};
static std::atomic<int> g_last_pos_1024{0};

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
		 * obs_frontend_release_tbar() sees tBar->value() == 1023 at completion and does not
		 * snap back to the old scene (OBSStudioAPI.cpp:115 vs OBSBasic_Transitions.cpp:752).
		 */
		g_tbar_slider->blockSignals(true);
		g_tbar_slider->setValue(pos_1024);
		g_tbar_slider->blockSignals(false);
		obs_frontend_set_tbar_position(pos_1024);
	}

	if (do_release) {
		obs_frontend_release_tbar();
	}
}

static void on_tbar_frontend_event(enum obs_frontend_event event, void *unused)
{
	UNUSED_PARAMETER(unused);
	switch (event) {
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
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
