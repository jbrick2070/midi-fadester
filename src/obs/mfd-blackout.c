/*
 * Midi Fadester
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "obs/mfd-blackout.h"

#include <obs-module.h>
#include <util/threading.h>
#include "plugin-support.h"

#include <math.h>

#define BLACKOUT_FIRST_CHANNEL (MAX_CHANNELS - 1) /* drawn last, on top of every other channel */
#define BLACKOUT_LAST_CHANNEL 8                   /* OBS uses 0..6; the Downstream Keyer plugin starts at 7 */
#define BLACKOUT_MIN_MS 300

static struct {
	bool ready;
	pthread_mutex_t lock;
	bool target;            /* black asked for */
	float level;            /* 0 clear .. 1 black */
	float rate;             /* level per second */
	float written;          /* level last written to the source, -1 none */
	obs_source_t *source;   /* the private black source, a reference of ours */
	int channel;            /* the output channel it is on, -1 none */
	uint32_t width, height; /* canvas size last written */
} g_bo = {.channel = -1, .written = -1.0f};

void mfd_blackout_init(void)
{
	if (g_bo.ready) {
		return;
	}
	pthread_mutex_init(&g_bo.lock, NULL);
	g_bo.ready = true;
}

void mfd_blackout_toggle(int fade_ms)
{
	if (!g_bo.ready) {
		return;
	}
	int ms = fade_ms < BLACKOUT_MIN_MS ? BLACKOUT_MIN_MS : fade_ms;
	pthread_mutex_lock(&g_bo.lock);
	g_bo.target = !g_bo.target;
	g_bo.rate = 1000.0f / (float)ms;
	bool on = g_bo.target;
	pthread_mutex_unlock(&g_bo.lock);
	obs_log(LOG_INFO, "Blackout: %s over %d ms", on ? "fading Program to black" : "fading Program back", ms);
}

bool mfd_blackout_is_on(void)
{
	if (!g_bo.ready) {
		return false;
	}
	pthread_mutex_lock(&g_bo.lock);
	bool on = g_bo.target || g_bo.level > 0.0f;
	pthread_mutex_unlock(&g_bo.lock);
	return on;
}

float mfd_blackout_level(void)
{
	if (!g_bo.ready) {
		return 0.0f;
	}
	pthread_mutex_lock(&g_bo.lock);
	float level = g_bo.level;
	pthread_mutex_unlock(&g_bo.lock);
	return level;
}

static void write_source(obs_source_t *src, float level, uint32_t w, uint32_t h)
{
	obs_data_t *settings = obs_data_create();
	/* colour_source_v3 reads ABGR: alpha in the top byte, black RGB */
	uint32_t alpha = (uint32_t)lroundf(level * 255.0f);
	obs_data_set_int(settings, "color", (long long)(alpha << 24));
	obs_data_set_int(settings, "width", (long long)w);
	obs_data_set_int(settings, "height", (long long)h);
	obs_source_update(src, settings);
	obs_data_release(settings);
}

void mfd_blackout_tick(float seconds)
{
	if (!g_bo.ready || !obs_initialized()) {
		return;
	}
	struct obs_video_info ovi;
	bool have_video = obs_get_video_info(&ovi);
	pthread_mutex_lock(&g_bo.lock);
	obs_source_t *src = NULL;
	float level = g_bo.level;
	uint32_t w = g_bo.width, h = g_bo.height;
	if (g_bo.source && g_bo.channel >= 0) {
		/* only moves once the source is on air: a fade never starts as a jump */
		float goal = g_bo.target ? 1.0f : 0.0f;
		float step = seconds * g_bo.rate;
		if (g_bo.level < goal) {
			g_bo.level = fminf(goal, g_bo.level + step);
		} else if (g_bo.level > goal) {
			g_bo.level = fmaxf(goal, g_bo.level - step);
		}
		level = g_bo.level;
		bool resized = have_video && (ovi.base_width != g_bo.width || ovi.base_height != g_bo.height);
		if (resized) {
			g_bo.width = w = ovi.base_width;
			g_bo.height = h = ovi.base_height;
		}
		if (resized || fabsf(level - g_bo.written) > 0.001f) {
			g_bo.written = level;
			src = obs_source_get_ref(g_bo.source);
		}
	}
	pthread_mutex_unlock(&g_bo.lock);
	if (src) {
		write_source(src, level, w, h);
		obs_source_release(src);
	}
}

/* Whether this channel holds this source; releases the reference obs_get_output_source gives. */
static bool channel_holds(int ch, obs_source_t *src)
{
	obs_source_t *now = obs_get_output_source((uint32_t)ch);
	bool same = now && now == src;
	obs_source_release(now);
	return same;
}

static int free_channel(void)
{
	for (int ch = BLACKOUT_FIRST_CHANNEL; ch >= BLACKOUT_LAST_CHANNEL; --ch) {
		obs_source_t *now = obs_get_output_source((uint32_t)ch);
		obs_source_release(now);
		if (!now) {
			return ch;
		}
	}
	return -1;
}

void mfd_blackout_ui(void)
{
	if (!g_bo.ready || !obs_initialized()) {
		return;
	}
	pthread_mutex_lock(&g_bo.lock);
	bool need = g_bo.target || g_bo.level > 0.0f;
	obs_source_t *src = g_bo.source ? obs_source_get_ref(g_bo.source) : NULL;
	int ch = g_bo.channel;
	pthread_mutex_unlock(&g_bo.lock);

	if (need && !src) {
		struct obs_video_info ovi;
		uint32_t w = 1920, h = 1080;
		if (obs_get_video_info(&ovi)) {
			w = ovi.base_width;
			h = ovi.base_height;
		}
		obs_data_t *settings = obs_data_create();
		obs_data_set_int(settings, "color", 0);
		obs_data_set_int(settings, "width", (long long)w);
		obs_data_set_int(settings, "height", (long long)h);
		obs_source_t *made = obs_source_create_private("color_source_v3", "Fadester Blackout", settings);
		obs_data_release(settings);
		if (!made) {
			obs_log(LOG_WARNING, "Blackout: OBS's colour source is not available, so Blackout cannot work");
			pthread_mutex_lock(&g_bo.lock);
			g_bo.target = false;
			pthread_mutex_unlock(&g_bo.lock);
			return;
		}
		int free_ch = free_channel();
		if (free_ch < 0) {
			obs_log(LOG_WARNING, "Blackout: every OBS output channel above the scene is in use");
			obs_source_release(made);
			pthread_mutex_lock(&g_bo.lock);
			g_bo.target = false;
			pthread_mutex_unlock(&g_bo.lock);
			return;
		}
		obs_set_output_source((uint32_t)free_ch, made);
		pthread_mutex_lock(&g_bo.lock);
		g_bo.source = made; /* our reference */
		g_bo.channel = free_ch;
		g_bo.width = w;
		g_bo.height = h;
		g_bo.written = 0.0f;
		pthread_mutex_unlock(&g_bo.lock);
		obs_log(LOG_INFO, "Blackout: black source on output channel %d (%ux%u)", free_ch, w, h);
		return;
	}
	if (!src) {
		return;
	}
	if (need && (ch < 0 || !channel_holds(ch, src))) {
		/* OBS (a scene collection reload) or another plugin took the channel: find another, keep the fade */
		int free_ch = free_channel();
		if (free_ch >= 0) {
			obs_set_output_source((uint32_t)free_ch, src);
			obs_log(LOG_INFO, "Blackout: output channel %d was taken, moved to channel %d", ch, free_ch);
		} else {
			obs_log(LOG_WARNING, "Blackout: every OBS output channel above the scene is in use, so the "
					     "blackout is let go");
		}
		pthread_mutex_lock(&g_bo.lock);
		g_bo.channel = free_ch;
		if (free_ch < 0) {
			g_bo.target = false; /* never claim black that is not on air */
			g_bo.level = 0.0f;
		}
		pthread_mutex_unlock(&g_bo.lock);
	} else if (!need) {
		/* clear again: take it off air and let it go, unless black was asked for again meanwhile */
		pthread_mutex_lock(&g_bo.lock);
		obs_source_t *mine = NULL;
		if (!g_bo.target && g_bo.level <= 0.0f && g_bo.source == src) {
			mine = g_bo.source;
			g_bo.source = NULL;
			g_bo.channel = -1;
			g_bo.written = -1.0f;
		}
		pthread_mutex_unlock(&g_bo.lock);
		if (mine) {
			if (ch >= 0 && channel_holds(ch, src)) {
				obs_set_output_source((uint32_t)ch, NULL);
			}
			obs_source_release(mine);
		}
	}
	obs_source_release(src);
}

void mfd_blackout_reset(void)
{
	if (!g_bo.ready) {
		return;
	}
	pthread_mutex_lock(&g_bo.lock);
	g_bo.target = false;
	g_bo.level = 0.0f;
	obs_source_t *mine = g_bo.source;
	int ch = g_bo.channel;
	g_bo.source = NULL;
	g_bo.channel = -1;
	g_bo.written = -1.0f;
	pthread_mutex_unlock(&g_bo.lock);
	if (mine) {
		if (obs_initialized() && ch >= 0 && channel_holds(ch, mine)) {
			obs_set_output_source((uint32_t)ch, NULL);
		}
		obs_source_release(mine);
	}
}

void mfd_blackout_shutdown(void)
{
	if (!g_bo.ready) {
		return;
	}
	mfd_blackout_reset();
	pthread_mutex_destroy(&g_bo.lock);
	g_bo.ready = false;
}
