/*
 * Midi Fadester
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*
 * macOS MIDI input through CoreMIDI, behind the same interface as the Windows WinMM backend
 * (midi/winmm-device.h), so the engine, the UI and the obs-websocket API stay unchanged.
 *
 * Messages are pushed to the engine's queue in WinMM's MIM_DATA shape: param1 packs
 * status | data1 << 8 | data2 << 16, param2 is milliseconds since the port was started.
 * CoreMIDI allows several apps on one port, so MFD_WINMM_ERR_BUSY_ALLOCATED never happens here.
 */

#include "midi/winmm-device.h"
#include "core/apc40-profile.h"
#include <obs-module.h>
#include <util/bmem.h>
#include <util/threading.h>
#include "plugin-support.h"

#include <CoreFoundation/CoreFoundation.h>
#include <CoreMIDI/CoreMIDI.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

struct mfd_coremidi_port {
	MIDIPortRef port;
	MIDIEndpointRef source;
	uint64_t start_host_time;
};

static MIDIClientRef g_client = 0;
static pthread_once_t g_client_once = PTHREAD_ONCE_INIT;
static void (*g_setup_changed)(void);
static mach_timebase_info_data_t g_timebase;

static void notify_setup_change(const MIDINotification *msg, void *context)
{
	(void)context;
	switch (msg->messageID) {
	case kMIDIMsgSetupChanged:
	case kMIDIMsgObjectAdded:
	case kMIDIMsgObjectRemoved:
		if (g_setup_changed)
			g_setup_changed();
		break;
	default:
		break;
	}
}

static void client_create(void)
{
	mach_timebase_info(&g_timebase);
	/* The engine creates the client on OBS's UI thread, whose run loop receives notifications. */
	OSStatus st = MIDIClientCreate(CFSTR("Midi Fadester"), notify_setup_change, NULL, &g_client);
	if (st != noErr) {
		g_client = 0;
		obs_log(LOG_WARNING, "CoreMIDI: MIDIClientCreate failed (OSStatus %d)", (int)st);
	}
}

static bool ensure_client(void)
{
	pthread_once(&g_client_once, client_create);
	return g_client != 0;
}

void mfd_coremidi_set_setup_callback(void (*callback)(void))
{
	g_setup_changed = callback;
}

static void endpoint_name(MIDIEndpointRef ep, char *out, size_t out_size)
{
	out[0] = '\0';
	CFStringRef name = NULL;
	if (MIDIObjectGetStringProperty(ep, kMIDIPropertyDisplayName, &name) != noErr || !name) {
		if (MIDIObjectGetStringProperty(ep, kMIDIPropertyName, &name) != noErr || !name) {
			return;
		}
	}
	if (!CFStringGetCString(name, out, (CFIndex)out_size, kCFStringEncodingUTF8)) {
		out[0] = '\0';
	}
	CFRelease(name);
}

size_t mfd_winmm_enumerate(struct mfd_midi_device_info *out_list, size_t max_devices)
{
	ensure_client();
	ItemCount num = MIDIGetNumberOfSources();
	if (!out_list || max_devices == 0U) {
		return (size_t)num;
	}

	size_t written = 0U;
	for (ItemCount i = 0; i < num && written < max_devices; ++i) {
		MIDIEndpointRef ep = MIDIGetSource(i);
		if (!ep) {
			continue;
		}
		struct mfd_midi_device_info *info = &out_list[written];
		info->index = (uint32_t)i;
		endpoint_name(ep, info->name, sizeof(info->name));
		if (info->name[0] == '\0') {
			continue;
		}
		info->detected_profile = mfd_profile_match_device_name(info->name);
		info->is_apc40_mk2 = (info->detected_profile == MFD_PROFILE_APC40_MK2);
		++written;
	}
	return written;
}

void mfd_winmm_init(struct mfd_winmm_device *dev, struct mfd_midi_queue *queue)
{
	if (!dev) {
		return;
	}
	ensure_client();
	memset(dev, 0, sizeof(*dev));
	dev->queue = queue;
	dev->error_kind = MFD_WINMM_OK;
	os_atomic_store_bool(&dev->device_closed_unexpectedly, false);
}

struct mfd_coremidi_out {
	MIDIPortRef port;
	MIDIEndpointRef dest;
};

/* The same desk's MIDI Out: the destination of the source's entity, else a destination with the source's name. */
static void open_output(struct mfd_winmm_device *dev, MIDIEndpointRef source)
{
	MIDIEndpointRef dest = 0;
	MIDIEntityRef entity = 0;
	if (MIDIEndpointGetEntity(source, &entity) == noErr && entity &&
	    MIDIEntityGetNumberOfDestinations(entity) > 0) {
		dest = MIDIEntityGetDestination(entity, 0);
	}
	char name[MFD_MIDI_DEV_NAME_MAX];
	for (ItemCount i = 0; !dest && i < MIDIGetNumberOfDestinations(); ++i) {
		MIDIEndpointRef ep = MIDIGetDestination(i);
		endpoint_name(ep, name, sizeof(name));
		if (ep && strcmp(name, dev->opened_name) == 0) {
			dest = ep;
		}
	}
	if (!dest) {
		obs_log(LOG_INFO, "No MIDI Out for '%s': the VJ buttons work unlit", dev->opened_name);
		return;
	}
	struct mfd_coremidi_out *o = bzalloc(sizeof(*o));
	o->dest = dest;
	OSStatus st = MIDIOutputPortCreate(g_client, CFSTR("Midi Fadester Out"), &o->port);
	if (st != noErr) {
		bfree(o);
		obs_log(LOG_INFO,
			"MIDI Out for '%s' could not be opened (CoreMIDI error %d): the VJ buttons work unlit",
			dev->opened_name, (int)st);
		return;
	}
	endpoint_name(dest, dev->out_name, sizeof(dev->out_name));
	dev->out_handle = o;
	obs_log(LOG_INFO, "Opened MIDI Out device '%s' for the button lights", dev->out_name);
}

static void close_output(struct mfd_winmm_device *dev)
{
	if (dev->out_handle) {
		struct mfd_coremidi_out *o = (struct mfd_coremidi_out *)dev->out_handle;
		dev->out_handle = NULL;
		MIDIPortDispose(o->port);
		bfree(o);
		obs_log(LOG_INFO, "Closed MIDI Out device '%s'", dev->out_name);
	}
	dev->out_name[0] = 0;
}

bool mfd_winmm_send_short(struct mfd_winmm_device *dev, uint8_t status, uint8_t data1, uint8_t data2)
{
	if (!dev || !dev->out_handle) {
		return false;
	}
	struct mfd_coremidi_out *o = (struct mfd_coremidi_out *)dev->out_handle;
	MIDIEventList list;
	MIDIEventPacket *pkt = MIDIEventListInit(&list, kMIDIProtocol_1_0);
	/* One UMP word: type 2 (MIDI 1.0 channel voice), group 0. */
	UInt32 word = 0x20000000U | ((UInt32)status << 16) | ((UInt32)(data1 & 0x7FU) << 8) | (UInt32)(data2 & 0x7FU);
	pkt = MIDIEventListAdd(&list, sizeof(list), pkt, 0, 1, &word);
	return pkt && MIDISendEventList(o->port, o->dest, &list) == noErr;
}

bool mfd_winmm_has_output(const struct mfd_winmm_device *dev)
{
	return dev && dev->out_handle != NULL;
}

void mfd_winmm_close(struct mfd_winmm_device *dev)
{
	if (!dev) {
		return;
	}
	close_output(dev);
	if (dev->handle) {
		struct mfd_coremidi_port *p = (struct mfd_coremidi_port *)dev->handle;
		dev->handle = NULL;
		dev->is_open = false;
		MIDIPortDisconnectSource(p->port, p->source);
		/* After MIDIPortDispose returns, CoreMIDI calls the receive block no more. */
		MIDIPortDispose(p->port);
		bfree(p);
		obs_log(LOG_INFO, "Closed MIDI In device '%s'", dev->opened_name);
	}
	dev->is_open = false;
	os_atomic_store_bool(&dev->device_closed_unexpectedly, false);
}

static uint32_t host_to_ms(uint64_t host_time)
{
	uint64_t ns = host_time * g_timebase.numer / g_timebase.denom;
	return (uint32_t)(ns / 1000000ULL);
}

static void receive_events(struct mfd_winmm_device *dev, struct mfd_coremidi_port *p, const MIDIEventList *list)
{
	if (!dev->queue) {
		return;
	}
	const MIDIEventPacket *pkt = &list->packet[0];
	for (UInt32 n = 0; n < list->numPackets; ++n) {
		uint64_t ts = pkt->timeStamp ? pkt->timeStamp : mach_absolute_time();
		uint32_t ms = ts > p->start_host_time ? host_to_ms(ts - p->start_host_time) : 0U;
		for (UInt32 w = 0; w < pkt->wordCount; ++w) {
			uint32_t word = pkt->words[w];
			uint32_t type = word >> 28;
			/* 0x1: system common / real-time, 0x2: MIDI 1.0 channel voice; both are one word. */
			if (type == 0x1U || type == 0x2U) {
				uint32_t status = (word >> 16) & 0xFFU;
				uint32_t d1 = (word >> 8) & 0x7FU;
				uint32_t d2 = word & 0x7FU;
				mfd_midi_queue_push(dev->queue, status | (d1 << 8) | (d2 << 16), ms);
			} else if (type == 0x3U || type == 0x4U) {
				++w; /* 64-bit packets (SysEx, MIDI 2.0 voice): skipped, as WinMM's MIM_LONGDATA is */
			} else if (type >= 0x5U) {
				w += 3U; /* 128-bit packets: skipped */
			}
		}
		pkt = MIDIEventPacketNext(pkt);
	}
}

bool mfd_winmm_open_by_name(struct mfd_winmm_device *dev, const char *device_name)
{
	if (!dev) {
		return false;
	}

	mfd_winmm_close(dev);
	dev->last_error[0] = '\0';
	dev->error_kind = MFD_WINMM_OK;
	dev->error_operation = MFD_WINMM_OP_NONE;
	dev->error_code = 0U;
	dev->error_device_name[0] = '\0';

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
			snprintf(dev->error_device_name, sizeof(dev->error_device_name), "%s", device_name);
			snprintf(dev->last_error, sizeof(dev->last_error),
				 "Configured MIDI device '%s' is not connected.", device_name);
			return false;
		}
	} else {
		for (size_t i = 0U; i < count && selected_idx < 0; ++i) {
			if (list[i].is_apc40_mk2) {
				selected_idx = (int)i;
			}
		}
		for (size_t i = 0U; i < count && selected_idx < 0; ++i) {
			if (list[i].detected_profile != MFD_PROFILE_GENERIC) {
				selected_idx = (int)i;
			}
		}
		if (selected_idx < 0) {
			selected_idx = 0;
		}
	}

	const char *chosen_name = list[selected_idx].name;
	snprintf(dev->opened_name, sizeof(dev->opened_name), "%s", chosen_name);

	MIDIEndpointRef source = MIDIGetSource((ItemCount)list[selected_idx].index);
	struct mfd_coremidi_port *p = bzalloc(sizeof(*p));
	p->source = source;

	OSStatus st = ensure_client() ? noErr : (OSStatus)kMIDINoConnection;
	if (st == noErr) {
		struct mfd_winmm_device *target = dev->callback_owner ? dev->callback_owner : dev;
		struct mfd_coremidi_port *port_state = p;
		st = MIDIInputPortCreateWithProtocol(g_client, CFSTR("Midi Fadester In"), kMIDIProtocol_1_0, &p->port,
						     ^(const MIDIEventList *evtlist, void *src_ref) {
							     (void)src_ref;
							     receive_events(target, port_state, evtlist);
						     });
	}
	if (st != noErr) {
		bfree(p);
		dev->error_kind = MFD_WINMM_ERR_OPEN_FAILED;
		dev->error_operation = MFD_WINMM_OP_OPEN;
		dev->error_code = (uint32_t)st;
		snprintf(dev->error_device_name, sizeof(dev->error_device_name), "%s", chosen_name);
		snprintf(dev->last_error, sizeof(dev->last_error), "Failed to open MIDI port '%s' (CoreMIDI error %d).",
			 chosen_name, (int)st);
		obs_log(LOG_WARNING, "%s", dev->last_error);
		return false;
	}

	p->start_host_time = mach_absolute_time();
	st = MIDIPortConnectSource(p->port, source, NULL);
	if (st != noErr) {
		MIDIPortDispose(p->port);
		bfree(p);
		dev->error_kind = MFD_WINMM_ERR_OPEN_FAILED;
		dev->error_operation = MFD_WINMM_OP_START;
		dev->error_code = (uint32_t)st;
		snprintf(dev->error_device_name, sizeof(dev->error_device_name), "%s", chosen_name);
		snprintf(dev->last_error, sizeof(dev->last_error),
			 "Failed to start MIDI input stream on '%s' (CoreMIDI error %d).", chosen_name, (int)st);
		obs_log(LOG_WARNING, "%s", dev->last_error);
		return false;
	}

	dev->handle = p;
	dev->is_open = true;
	dev->error_kind = MFD_WINMM_OK;
	os_atomic_store_bool(&dev->device_closed_unexpectedly, false);
	obs_log(LOG_INFO, "Opened MIDI In device '%s' (CoreMIDI source %u)", dev->opened_name,
		(unsigned)list[selected_idx].index);
	/* Only an APC40 mkII has lights to drive; another desk's MIDI Out stays free for other apps. */
	if (mfd_is_apc40_mk2_name(dev->opened_name)) {
		open_output(dev, source);
	}
	return true;
}

const char *mfd_winmm_error_slug(enum mfd_winmm_error_kind kind)
{
	switch (kind) {
	case MFD_WINMM_ERR_NO_DEVICES:
		return "no_devices";
	case MFD_WINMM_ERR_CONFIGURED_NOT_FOUND:
		return "device_not_found";
	case MFD_WINMM_ERR_BUSY_ALLOCATED:
		return "device_busy";
	case MFD_WINMM_ERR_DISCONNECTED:
		return "device_disconnected";
	case MFD_WINMM_ERR_OPEN_FAILED:
		return "device_open_failed";
	case MFD_WINMM_OK:
	default:
		return "";
	}
}

bool mfd_winmm_is_present(const struct mfd_winmm_device *dev)
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

	/* An unplugged USB device's endpoint can linger offline; treat offline as gone. */
	if (found && dev->handle) {
		SInt32 offline = 0;
		const struct mfd_coremidi_port *p = (const struct mfd_coremidi_port *)dev->handle;
		if (MIDIObjectGetIntegerProperty(p->source, kMIDIPropertyOffline, &offline) == noErr && offline) {
			found = false;
		}
	}
	return found;
}
