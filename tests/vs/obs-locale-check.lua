-- visual-sidechain
-- Copyright (C) 2026 Jeffrey A. Brick
-- SPDX-License-Identifier: GPL-2.0-or-later
--
-- Translations in OBS (R-I18N-1, R-I18N-2; TESTING.md "Translations", T.1/T.2): with OBS running in another language,
-- a Shader Filter's and an Audio Modulator's Properties show that language's text, including a shader error line built
-- through vs_text_format and a file dialog filter name. VS_LOCALE_FILE names the language file (UTF-8) the expected
-- texts are read from, as os.getenv on Windows would garble them. Run inside portable OBS started in that language (for
-- example by obs-playlister's tests/obs/run-harness.ps1 -Language de-DE -Script <path to this file>). Logs
-- "HARNESS PASS|FAIL <check>" and "HARNESS DONE".

obs = obslua

local TEMP = (os.getenv("TEMP") or "."):gsub("\\", "/")
local MISSING_SHADER = TEMP .. "/vs-locale-missing.shader"

local function read_locale(path)
	local texts = {}
	local file = path and io.open(path, "rb")
	if not file then
		return texts
	end
	for line in file:lines() do
		local key, text = line:match('^([^#=%s][^=]-)%s*=%s*"(.*)"%s*\r?$')
		if key then
			texts[key] = text
		end
	end
	file:close()
	return texts
end

local T = read_locale(os.getenv("VS_LOCALE_FILE"))

local color, shader, modulator, item = nil, nil, nil, nil
local started = 0

local function log(s)
	obs.script_log(obs.LOG_INFO, "HARNESS " .. s)
end

local function check(name, ok, detail)
	log((ok and "PASS " or "FAIL ") .. name .. " (" .. (detail or "") .. ")")
end

local function now_ms()
	return obs.os_gettime_ns() / 1000000
end

-- the text of a format before its first conversion: what any filled-in line starts with
local function format_head(text)
	return (text or ""):match("^(.-)%%") or (text or "")
end

local function contains(haystack, needle)
	return needle ~= "" and (haystack or ""):find(needle, 1, true) ~= nil
end

local function create_sources()
	local settings = obs.obs_data_create()
	obs.obs_data_set_int(settings, "width", 320)
	obs.obs_data_set_int(settings, "height", 180)
	color = obs.obs_source_create("color_source_v3", "vs-locale-color", settings, nil)
	obs.obs_data_release(settings)
	local scene_source = obs.obs_frontend_get_current_scene()
	item = obs.obs_scene_add(obs.obs_scene_from_source(scene_source), color)
	obs.obs_source_release(scene_source)

	settings = obs.obs_data_create()
	obs.obs_data_set_string(settings, "preset_shader", "__custom_file__")
	obs.obs_data_set_string(settings, "shader_file_path", MISSING_SHADER)
	shader = obs.obs_source_create("visual_sidechain_shader_filter", "vs-locale-shader", settings, nil)
	obs.obs_data_release(settings)
	obs.obs_source_filter_add(color, shader)

	modulator = obs.obs_source_create("visual_sidechain_modulator_filter", "vs-locale-modulator", nil, nil)
	obs.obs_source_filter_add(color, modulator)
end

local function finish()
	check("the expected texts came from the language file",
		(T["Shader.Preset"] or "") ~= "" and (T["Shader.Error.FileNotFound"] or "") ~= "", T["Shader.Preset"])

	local props = obs.obs_source_properties(shader)
	local preset = obs.obs_properties_get(props, "preset_shader")
	local preset_label = preset and obs.obs_property_description(preset) or ""
	check("a Shader Filter label is in the language OBS runs in", preset_label == T["Shader.Preset"], preset_label)
	local path = obs.obs_properties_get(props, "shader_file_path")
	local filter = path and obs.obs_property_path_filter(path) or ""
	check("the file dialog filter has the translated name and the fixed patterns",
		contains(filter, T["Shader.FileFilter.Shaders"] .. " (*.shader *.effect *.hlsl);;"), filter)
	local status = obs.obs_properties_get(props, "compile_status")
	local status_label = status and obs.obs_property_description(status) or ""
	check("the shader error line's label is translated", status_label == T["Shader.CompileStatus"], status_label)
	obs.obs_properties_destroy(props)

	local settings = obs.obs_source_get_settings(shader)
	local error_line = obs.obs_data_get_string(settings, "compile_status")
	obs.obs_data_release(settings)
	check("the shader error line, a vs_text_format text, is translated and filled in",
		contains(error_line, format_head(T["Shader.Error.FileNotFound"]) .. MISSING_SHADER), error_line)

	props = obs.obs_source_properties(modulator)
	local group = obs.obs_properties_get(props, "mod_slot_group_1")
	local group_label = group and obs.obs_property_description(group) or ""
	check("an Audio Modulator group title is translated", group_label == T["Modulator.SlotGroupPrefix"] .. " 1",
		group_label)
	obs.obs_properties_destroy(props)

	obs.obs_source_filter_remove(color, shader)
	obs.obs_source_filter_remove(color, modulator)
	obs.obs_source_release(shader)
	obs.obs_source_release(modulator)
	obs.obs_sceneitem_remove(item)
	obs.obs_source_release(color)
	shader, modulator, item, color = nil, nil, nil, nil
	log("DONE")
end

local function tick()
	if not color then
		create_sources()
		started = now_ms()
		return
	end
	if now_ms() - started >= 3000 then
		obs.timer_remove(tick)
		finish()
	end
end

function script_description()
	return "visual-sidechain translations check"
end

function script_load(settings)
	os.remove(MISSING_SHADER)
	obs.timer_add(tick, 100)
end
