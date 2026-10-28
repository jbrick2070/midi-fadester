/*
 * Midi Fadester
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "core/apc40-profile.h"
#include "core/midi-queue.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MFD_MAX_MIDI_DEVICES 32
#define MFD_MIDI_DEV_NAME_MAX 128
#define MFD_MIDI_ERROR_MAX 256

enum mfd_winmm_error_kind {
	MFD_WINMM_OK = 0,
	MFD_WINMM_ERR_NO_DEVICES = 1,
	MFD_WINMM_ERR_CONFIGURED_NOT_FOUND = 2,
	MFD_WINMM_ERR_BUSY_ALLOCATED = 3,
	MFD_WINMM_ERR_DISCONNECTED = 4,
	MFD_WINMM_ERR_OPEN_FAILED = 5,
};

struct mfd_midi_device_info {
	uint32_t index;
	char name[MFD_MIDI_DEV_NAME_MAX];
	bool is_apc40_mk2;
	enum mfd_controller_profile detected_profile;
};

struct mfd_winmm_device {
	void *handle; /* HMIDIIN */
	struct mfd_midi_queue *queue;
	bool is_open;
	enum mfd_winmm_error_kind error_kind;
	char opened_name[MFD_MIDI_DEV_NAME_MAX];
	char last_error[MFD_MIDI_ERROR_MAX];
	volatile bool device_closed_unexpectedly;
};

/*
 * Enumerates currently connected WinMM MIDI In devices.
 * Returns the number of devices written to out_list (up to max_devices).
 */
size_t mfd_winmm_enumerate(struct mfd_midi_device_info *out_list, size_t max_devices);

void mfd_winmm_init(struct mfd_winmm_device *dev, struct mfd_midi_queue *queue);
void mfd_winmm_close(struct mfd_winmm_device *dev);

/*
 * Opens a WinMM MIDI In port matching device_name (or the first APC40 mkII / first port if device_name is empty).
 * If another app holds exclusive access (MMSYSERR_ALLOCATED), returns false and populates dev->last_error
 * and dev->error_kind with an explicit actionable classification for the UI banner.
 */
bool mfd_winmm_open_by_name(struct mfd_winmm_device *dev, const char *device_name);

/*
 * Checks if the opened device is still present in WinMM enumeration; if unplugged, closes the handle
 * and sets dev->last_error and dev->error_kind so the UI shows a disconnected status without crashing.
 */
bool mfd_winmm_check_health(struct mfd_winmm_device *dev);

#ifdef __cplusplus
}
#endif
