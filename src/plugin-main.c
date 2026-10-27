/*
 * Midi Fadester
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <obs-module.h>
#include <plugin-support.h>
#include "obs/mfd-layer-filter.h"
#include "obs/mfd-engine.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

const char *obs_module_name(void)
{
	return obs_module_text("PluginDisplayName");
}

const char *obs_module_description(void)
{
	return obs_module_text("PluginDescription");
}

bool obs_module_load(void)
{
	mfd_register_layer_filter();
	mfd_engine_init();
	obs_log(LOG_INFO, "plugin loaded successfully (version %s)", PLUGIN_VERSION);
	return true;
}

void obs_module_unload(void)
{
	mfd_engine_shutdown();
	obs_log(LOG_INFO, "plugin unloaded");
}
