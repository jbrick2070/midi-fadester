/*
 * Midi Fadester
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <obs-module.h>
#include "plugin-support.h"

#include "core/mfd-text.h"
#include "obs/mfd-layer-filter.h"
#include "obs/mfd-engine.h"
#include "obs/mfd-tbar.h"
#include "obs/mfd-vendor-api.h"
#include "ui/mfd-settings-dialog.hpp"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

/* The locale's text for key, or NULL when OBS has none (then the English in the code is used, R-I18N-1). */
static const char *locale_text(const char *key)
{
	const char *text = NULL;
	return (obs_module_get_string(key, &text) && text) ? text : NULL;
}

MODULE_EXPORT const char *obs_module_name(void)
{
	/* the product name stays as it is in every language (R-I18N-4) */
	return "Midi Fadester";
}

MODULE_EXPORT const char *obs_module_description(void)
{
	return mfd_text("PluginDescription", "MIDI hardware fader & video mixer engine for OBS Studio");
}

bool obs_module_load(void)
{
	/* before any consumer starts (R-THR-1) */
	mfd_text_set_lookup(locale_text);
	mfd_register_layer_filter();
	if (!mfd_engine_init()) {
		obs_log(LOG_ERROR, "Midi Fadester failed to initialize engine state mutex");
		mfd_text_set_lookup(NULL);
		return false;
	}
	mfd_tbar_init();
	mfd_ui_init();

	obs_log(LOG_INFO, "plugin loaded successfully: Midi Fadester %s (commit %s, built %s)", PLUGIN_VERSION,
		PLUGIN_GIT_COMMIT, PLUGIN_BUILT_AT);
	return true;
}

void obs_module_post_load(void)
{
	mfd_vendor_api_register(mfd_engine_get());
}

void obs_module_unload(void)
{
	mfd_vendor_api_shutdown(mfd_engine_get());
	mfd_ui_shutdown();
	mfd_tbar_shutdown();
	mfd_engine_shutdown();
	/* after every consumer has stopped */
	mfd_text_set_lookup(NULL);
	obs_log(LOG_INFO, "plugin unloaded");
}
