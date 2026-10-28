/*
 * Midi Fadester
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "midi/winmm-device.h"
#include "core/apc40-profile.h"
#include <obs-module.h>
#include <util/threading.h>
#include "plugin-support.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>
#include <string.h>

static void wide_to_utf8(const WCHAR *wstr, char *out_utf8, size_t out_size)
{
	if (!out_utf8 || out_size == 0U) {
		return;
	}
	out_utf8[0] = '\0';
	if (!wstr) {
		return;
	}
	int written = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, out_utf8, (int)out_size, NULL, NULL);
	if (written <= 0) {
		out_utf8[0] = '\0';
	} else {
		out_utf8[out_size - 1U] = '\0';
	}
}

static void CALLBACK winmm_in_proc(HMIDIIN hMidiIn, UINT wMsg, DWORD_PTR dwInstance, DWORD_PTR dwParam1,
				   DWORD_PTR dwParam2)
{
	UNUSED_PARAMETER(hMidiIn);
	struct mfd_winmm_device *dev = (struct mfd_winmm_device *)dwInstance;
	if (!dev) {
		return;
	}

	if (wMsg == MIM_DATA) {
		if (dev->queue) {
			mfd_midi_queue_push(dev->queue, (uint32_t)dwParam1, (uint32_t)dwParam2);
		}
	} else if (wMsg == MIM_CLOSE) {
		os_atomic_store_bool(&dev->device_closed_unexpectedly, true);
	}
}

size_t mfd_winmm_enumerate(struct mfd_midi_device_info *out_list, size_t max_devices)
{
	UINT num_devs = midiInGetNumDevs();
	if (!out_list || max_devices == 0U) {
		return (size_t)num_devs;
	}

	size_t written = 0U;
	for (UINT i = 0U; i < num_devs && written < max_devices; ++i) {
		MIDIINCAPSW caps;
		memset(&caps, 0, sizeof(caps));
		if (midiInGetDevCapsW(i, &caps, sizeof(caps)) == MMSYSERR_NOERROR) {
			out_list[written].index = (uint32_t)i;
			wide_to_utf8(caps.szPname, out_list[written].name, sizeof(out_list[written].name));
			out_list[written].detected_profile = mfd_profile_match_device_name(out_list[written].name);
			out_list[written].is_apc40_mk2 = (out_list[written].detected_profile == MFD_PROFILE_APC40_MK2);
			++written;
		}
	}
	return written;
}

void mfd_winmm_init(struct mfd_winmm_device *dev, struct mfd_midi_queue *queue)
{
	if (!dev) {
		return;
	}
	memset(dev, 0, sizeof(*dev));
	dev->queue = queue;
	dev->error_kind = MFD_WINMM_OK;
	os_atomic_store_bool(&dev->device_closed_unexpectedly, false);
}

void mfd_winmm_close(struct mfd_winmm_device *dev)
{
	if (!dev) {
		return;
	}
	if (dev->handle) {
		HMIDIIN hmi = (HMIDIIN)dev->handle;
		dev->handle = NULL;
		dev->is_open = false;
		midiInStop(hmi);
		midiInReset(hmi);
		midiInClose(hmi);
		obs_log(LOG_INFO, "Closed MIDI In device '%s'", dev->opened_name);
	}
	dev->is_open = false;
	os_atomic_store_bool(&dev->device_closed_unexpectedly, false);
}

bool mfd_winmm_open_by_name(struct mfd_winmm_device *dev, const char *device_name)
{
	if (!dev) {
		return false;
	}

	mfd_winmm_close(dev);
	dev->last_error[0] = '\0';
	dev->error_kind = MFD_WINMM_OK;

	struct mfd_midi_device_info list[MFD_MAX_MIDI_DEVICES];
	size_t count = mfd_winmm_enumerate(list, MFD_MAX_MIDI_DEVICES);
	if (count == 0U) {
		dev->error_kind = (device_name && device_name[0] != '\0') ? MFD_WINMM_ERR_CONFIGURED_NOT_FOUND
									  : MFD_WINMM_ERR_NO_DEVICES;
		snprintf(
			dev->last_error, sizeof(dev->last_error),
			"No MIDI input devices detected. Connect your MIDI controller (e.g. Akai APC40 mkII) and click Reconnect.");
		return false;
	}

	int selected_idx = -1;
	if (device_name && device_name[0] != '\0') {
		for (size_t i = 0U; i < count; ++i) {
			if (strcmp(list[i].name, device_name) == 0) {
				selected_idx = (int)i;
				break;
			}
		}
		if (selected_idx < 0) {
			dev->error_kind = MFD_WINMM_ERR_CONFIGURED_NOT_FOUND;
			snprintf(dev->last_error, sizeof(dev->last_error),
				 "Configured MIDI device '%s' is not connected.", device_name);
			return false;
		}
	} else {
		for (size_t i = 0U; i < count; ++i) {
			if (list[i].is_apc40_mk2) {
				selected_idx = (int)i;
				break;
			}
		}
		if (selected_idx < 0) {
			for (size_t i = 0U; i < count; ++i) {
				if (list[i].detected_profile != MFD_PROFILE_GENERIC) {
					selected_idx = (int)i;
					break;
				}
			}
		}
		if (selected_idx < 0) {
			selected_idx = 0;
		}
	}

	UINT winmm_id = list[selected_idx].index;
	const char *chosen_name = list[selected_idx].name;
	snprintf(dev->opened_name, sizeof(dev->opened_name), "%s", chosen_name);

	HMIDIIN hmi = NULL;
	MMRESULT res = midiInOpen(&hmi, winmm_id, (DWORD_PTR)winmm_in_proc, (DWORD_PTR)dev, CALLBACK_FUNCTION);
	if (res != MMSYSERR_NOERROR) {
		if (res == MMSYSERR_ALLOCATED) {
			dev->error_kind = MFD_WINMM_ERR_BUSY_ALLOCATED;
			snprintf(
				dev->last_error, sizeof(dev->last_error),
				"MIDI port '%s' is busy (MMSYSERR_ALLOCATED). Windows WinMM allows only one application at a time — close Ableton Live, MIDI-OX, or your DAW and click Reconnect.",
				chosen_name);
		} else {
			dev->error_kind = MFD_WINMM_ERR_OPEN_FAILED;
			snprintf(dev->last_error, sizeof(dev->last_error),
				 "Failed to open MIDI port '%s' (WinMM error code %u).", chosen_name, (unsigned)res);
		}
		obs_log(LOG_WARNING, "%s", dev->last_error);
		return false;
	}

	MMRESULT start_res = midiInStart(hmi);
	if (start_res != MMSYSERR_NOERROR) {
		midiInClose(hmi);
		dev->error_kind = MFD_WINMM_ERR_OPEN_FAILED;
		snprintf(dev->last_error, sizeof(dev->last_error),
			 "Failed to start MIDI input stream on '%s' (WinMM error code %u).", chosen_name,
			 (unsigned)start_res);
		obs_log(LOG_WARNING, "%s", dev->last_error);
		return false;
	}

	dev->handle = (void *)hmi;
	dev->is_open = true;
	dev->error_kind = MFD_WINMM_OK;
	os_atomic_store_bool(&dev->device_closed_unexpectedly, false);
	obs_log(LOG_INFO, "Opened MIDI In device '%s' (WinMM index %u)", dev->opened_name, (unsigned)winmm_id);
	return true;
}

bool mfd_winmm_check_health(struct mfd_winmm_device *dev)
{
	if (!dev || !dev->is_open) {
		return false;
	}

	struct mfd_midi_device_info list[MFD_MAX_MIDI_DEVICES];
	size_t count = mfd_winmm_enumerate(list, MFD_MAX_MIDI_DEVICES);
	bool found = false;
	for (size_t i = 0U; i < count; ++i) {
		if (strcmp(list[i].name, dev->opened_name) == 0) {
			found = true;
			break;
		}
	}

	if (!found || os_atomic_load_bool(&dev->device_closed_unexpectedly)) {
		char lost_name[MFD_MIDI_DEV_NAME_MAX];
		snprintf(lost_name, sizeof(lost_name), "%s", dev->opened_name);
		mfd_winmm_close(dev);
		dev->error_kind = MFD_WINMM_ERR_DISCONNECTED;
		snprintf(dev->last_error, sizeof(dev->last_error),
			 "MIDI device '%s' was disconnected. Reconnect USB cable and click Reconnect.", lost_name);
		obs_log(LOG_WARNING, "%s", dev->last_error);
		return false;
	}

	return true;
}
