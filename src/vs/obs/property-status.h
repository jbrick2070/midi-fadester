/*
 * Visual Sidechain - Properties Status Helpers
 * Copyright (C) 2026 Jeffrey Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <obs-module.h>
#include <util/dstr.h>

#include <string.h>

/* Qt auto-detects rich text in info labels. Escape user-supplied names and
 * explicitly wrap the text so names containing '<' or '&' remain literal. */
static inline void vs_property_info_set_text(obs_property_t *property, const char *text)
{
	if (!property)
		return;
	struct dstr escaped = {0};
	dstr_cat(&escaped, "<html><body>");
	for (const char *p = text ? text : ""; *p; p++) {
		if (*p == '&')
			dstr_cat(&escaped, "&amp;");
		else if (*p == '<')
			dstr_cat(&escaped, "&lt;");
		else if (*p == '>')
			dstr_cat(&escaped, "&gt;");
		else
			dstr_cat_ch(&escaped, *p);
	}
	dstr_cat(&escaped, "</body></html>");
	obs_property_set_description(property, escaped.array);
	dstr_free(&escaped);
}

static inline obs_property_t *vs_properties_add_info(obs_properties_t *props, const char *name, const char *text)
{
	obs_property_t *property = obs_properties_add_text(props, name, "", OBS_TEXT_INFO);
	obs_property_text_set_info_word_wrap(property, true);
	vs_property_info_set_text(property, text);
	return property;
}

static inline const char *vs_property_string_choice_name(obs_property_t *property, const char *value)
{
	if (!property || !value)
		return NULL;
	for (size_t i = 0; i < obs_property_list_item_count(property); i++) {
		const char *candidate = obs_property_list_item_string(property, i);
		if (candidate && strcmp(candidate, value) == 0)
			return obs_property_list_item_name(property, i);
	}
	return NULL;
}

static inline const char *vs_property_int_choice_name(obs_property_t *property, long long value)
{
	if (!property)
		return NULL;
	for (size_t i = 0; i < obs_property_list_item_count(property); i++) {
		if (obs_property_list_item_int(property, i) == value)
			return obs_property_list_item_name(property, i);
	}
	return NULL;
}
