/*
 * Midi Fadester
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "core/apc40-profile.h"
#include <ctype.h>
#include <string.h>

static const char *const k_apc40_usb_tokens[] = {"APC40 mkII", "APC40 mk2", "APC40", NULL};
static const char *const k_apc_mini_usb_tokens[] = {"APC mini mk2", "APC mini", NULL};
static const char *const k_midimix_usb_tokens[] = {"MIDI Mix", "MIDImix", NULL};
static const char *const k_nanokontrol2_usb_tokens[] = {"nanoKONTROL2", NULL};
static const char *const k_launchcontrolxl_usb_tokens[] = {"Launch Control XL", NULL};

/*
 * Declarative Controller Profile Registry.
 * To add a new MIDI controller: append one mfd_controller_profile_def entry below with its
 * support tier (VERIFIED once tested on physical hardware, BETA_UNTESTED when mapped from
 * manufacturer MIDI documentation), WinMM port-name match substrings, physical topology flags,
 * and default 10-slot MIDI CC bindings.
 */
static const struct mfd_controller_profile_def k_profile_registry[] = {
	{
		MFD_PROFILE_GENERIC,
		"generic",
		"Generic MIDI (Custom)",
		"Generic MIDI",
		MFD_PROFILE_TIER_CUSTOM,
		"Custom MIDI layout — click Set up or Learn on any strip to bind your controller's faders or knobs.",
		true,
		8,
		true,
		0,
		MFD_DESK_UPPER_NONE,
		NULL,
		{
			{"crossfader",
			 "Crossfader (A/B)",
			 "crossfader",
			 {MFD_MIDI_NONE, 0U, 0U, false},
			 MFD_TARGET_UNASSIGNED,
			 MFD_CURVE_DIP_FREE},
			{"fader_1",
			 "Track Fader 1",
			 "fader",
			 {MFD_MIDI_NONE, 0U, 0U, false},
			 MFD_TARGET_UNASSIGNED,
			 MFD_CURVE_DIP_FREE},
			{"fader_2",
			 "Track Fader 2",
			 "fader",
			 {MFD_MIDI_NONE, 0U, 0U, false},
			 MFD_TARGET_UNASSIGNED,
			 MFD_CURVE_DIP_FREE},
			{"fader_3",
			 "Track Fader 3",
			 "fader",
			 {MFD_MIDI_NONE, 0U, 0U, false},
			 MFD_TARGET_UNASSIGNED,
			 MFD_CURVE_DIP_FREE},
			{"fader_4",
			 "Track Fader 4",
			 "fader",
			 {MFD_MIDI_NONE, 0U, 0U, false},
			 MFD_TARGET_UNASSIGNED,
			 MFD_CURVE_DIP_FREE},
			{"fader_5",
			 "Track Fader 5",
			 "fader",
			 {MFD_MIDI_NONE, 0U, 0U, false},
			 MFD_TARGET_UNASSIGNED,
			 MFD_CURVE_DIP_FREE},
			{"fader_6",
			 "Track Fader 6",
			 "fader",
			 {MFD_MIDI_NONE, 0U, 0U, false},
			 MFD_TARGET_UNASSIGNED,
			 MFD_CURVE_DIP_FREE},
			{"fader_7",
			 "Track Fader 7",
			 "fader",
			 {MFD_MIDI_NONE, 0U, 0U, false},
			 MFD_TARGET_UNASSIGNED,
			 MFD_CURVE_DIP_FREE},
			{"fader_8",
			 "Track Fader 8",
			 "fader",
			 {MFD_MIDI_NONE, 0U, 0U, false},
			 MFD_TARGET_UNASSIGNED,
			 MFD_CURVE_DIP_FREE},
			{"master",
			 "Master Fader",
			 "master",
			 {MFD_MIDI_NONE, 0U, 0U, false},
			 MFD_TARGET_UNASSIGNED,
			 MFD_CURVE_DIP_FREE},
			{"fx_1", "FX 1", "fx", {MFD_MIDI_NONE, 0U, 0U, false}, MFD_TARGET_UNASSIGNED, MFD_CURVE_LINEAR},
			{"fx_2", "FX 2", "fx", {MFD_MIDI_NONE, 0U, 0U, false}, MFD_TARGET_UNASSIGNED, MFD_CURVE_LINEAR},
			{"fx_3", "FX 3", "fx", {MFD_MIDI_NONE, 0U, 0U, false}, MFD_TARGET_UNASSIGNED, MFD_CURVE_LINEAR},
			{"fx_4", "FX 4", "fx", {MFD_MIDI_NONE, 0U, 0U, false}, MFD_TARGET_UNASSIGNED, MFD_CURVE_LINEAR},
			{"fx_5", "FX 5", "fx", {MFD_MIDI_NONE, 0U, 0U, false}, MFD_TARGET_UNASSIGNED, MFD_CURVE_LINEAR},
			{"fx_6", "FX 6", "fx", {MFD_MIDI_NONE, 0U, 0U, false}, MFD_TARGET_UNASSIGNED, MFD_CURVE_LINEAR},
			{"fx_7", "FX 7", "fx", {MFD_MIDI_NONE, 0U, 0U, false}, MFD_TARGET_UNASSIGNED, MFD_CURVE_LINEAR},
			{"fx_8", "FX 8", "fx", {MFD_MIDI_NONE, 0U, 0U, false}, MFD_TARGET_UNASSIGNED, MFD_CURVE_LINEAR},
		},
	},
	{
		MFD_PROFILE_APC40_MK2,
		"apc40mk2",
		"Akai APC40 mkII",
		"APC40 mkII",
		MFD_PROFILE_TIER_VERIFIED,
		"Hardware-verified on physical Akai APC40 mkII (Crossfader Ch 1 CC 15, Faders 1-8 Ch 1-8 CC 7, Master Ch 1 CC 14, Device Knobs 1-8 Ch 1 CC 16-23).",
		true,
		8,
		true,
		8,
		MFD_DESK_UPPER_CLIP_PADS,
		k_apc40_usb_tokens,
		{
			{"crossfader",
			 "Crossfader (A/B)",
			 "crossfader",
			 {MFD_MIDI_CC, 0U, 15U, false},
			 MFD_TARGET_TBAR,
			 MFD_CURVE_DIP_FREE},
			{"fader_1",
			 "Track Fader 1",
			 "fader",
			 {MFD_MIDI_CC, 0U, 7U, false},
			 MFD_TARGET_UNASSIGNED,
			 MFD_CURVE_DIP_FREE},
			{"fader_2",
			 "Track Fader 2",
			 "fader",
			 {MFD_MIDI_CC, 1U, 7U, false},
			 MFD_TARGET_UNASSIGNED,
			 MFD_CURVE_DIP_FREE},
			{"fader_3",
			 "Track Fader 3",
			 "fader",
			 {MFD_MIDI_CC, 2U, 7U, false},
			 MFD_TARGET_UNASSIGNED,
			 MFD_CURVE_DIP_FREE},
			{"fader_4",
			 "Track Fader 4",
			 "fader",
			 {MFD_MIDI_CC, 3U, 7U, false},
			 MFD_TARGET_UNASSIGNED,
			 MFD_CURVE_DIP_FREE},
			{"fader_5",
			 "Track Fader 5",
			 "fader",
			 {MFD_MIDI_CC, 4U, 7U, false},
			 MFD_TARGET_UNASSIGNED,
			 MFD_CURVE_DIP_FREE},
			{"fader_6",
			 "Track Fader 6",
			 "fader",
			 {MFD_MIDI_CC, 5U, 7U, false},
			 MFD_TARGET_UNASSIGNED,
			 MFD_CURVE_DIP_FREE},
			{"fader_7",
			 "Track Fader 7",
			 "fader",
			 {MFD_MIDI_CC, 6U, 7U, false},
			 MFD_TARGET_UNASSIGNED,
			 MFD_CURVE_DIP_FREE},
			{"fader_8",
			 "Track Fader 8",
			 "fader",
			 {MFD_MIDI_CC, 7U, 7U, false},
			 MFD_TARGET_UNASSIGNED,
			 MFD_CURVE_DIP_FREE},
			{"master",
			 "Master Fader",
			 "master",
			 {MFD_MIDI_CC, 0U, 14U, false},
			 MFD_TARGET_UNASSIGNED,
			 MFD_CURVE_DIP_FREE},
			{"fx_1", "FX 1", "fx", {MFD_MIDI_CC, 0U, 16U, false}, MFD_TARGET_UNASSIGNED, MFD_CURVE_LINEAR},
			{"fx_2", "FX 2", "fx", {MFD_MIDI_CC, 0U, 17U, false}, MFD_TARGET_UNASSIGNED, MFD_CURVE_LINEAR},
			{"fx_3", "FX 3", "fx", {MFD_MIDI_CC, 0U, 18U, false}, MFD_TARGET_UNASSIGNED, MFD_CURVE_LINEAR},
			{"fx_4", "FX 4", "fx", {MFD_MIDI_CC, 0U, 19U, false}, MFD_TARGET_UNASSIGNED, MFD_CURVE_LINEAR},
			{"fx_5", "FX 5", "fx", {MFD_MIDI_CC, 0U, 20U, false}, MFD_TARGET_UNASSIGNED, MFD_CURVE_LINEAR},
			{"fx_6", "FX 6", "fx", {MFD_MIDI_CC, 0U, 21U, false}, MFD_TARGET_UNASSIGNED, MFD_CURVE_LINEAR},
			{"fx_7", "FX 7", "fx", {MFD_MIDI_CC, 0U, 22U, false}, MFD_TARGET_UNASSIGNED, MFD_CURVE_LINEAR},
			{"fx_8", "FX 8", "fx", {MFD_MIDI_CC, 0U, 23U, false}, MFD_TARGET_UNASSIGNED, MFD_CURVE_LINEAR},
		},
	},
	{
		MFD_PROFILE_APC_MINI_MK2,
		"apcminimk2",
		"Akai APC mini mk2",
		"APC mini mk2",
		MFD_PROFILE_TIER_BETA_UNTESTED,
		"Beta (untested on hardware) — mapped from Akai APC mini mk2 factory MIDI spec (Faders 1-8: Ch 1 CC 48-55, Master: Ch 1 CC 56).",
		false,
		8,
		true,
		0,
		MFD_DESK_UPPER_CLIP_PADS,
		k_apc_mini_usb_tokens,
		{
			{"crossfader",
			 "Crossfader (A/B)",
			 "crossfader",
			 {MFD_MIDI_NONE, 0U, 0U, false},
			 MFD_TARGET_UNASSIGNED,
			 MFD_CURVE_DIP_FREE},
			{"fader_1",
			 "Track Fader 1",
			 "fader",
			 {MFD_MIDI_CC, 0U, 48U, false},
			 MFD_TARGET_UNASSIGNED,
			 MFD_CURVE_DIP_FREE},
			{"fader_2",
			 "Track Fader 2",
			 "fader",
			 {MFD_MIDI_CC, 0U, 49U, false},
			 MFD_TARGET_UNASSIGNED,
			 MFD_CURVE_DIP_FREE},
			{"fader_3",
			 "Track Fader 3",
			 "fader",
			 {MFD_MIDI_CC, 0U, 50U, false},
			 MFD_TARGET_UNASSIGNED,
			 MFD_CURVE_DIP_FREE},
			{"fader_4",
			 "Track Fader 4",
			 "fader",
			 {MFD_MIDI_CC, 0U, 51U, false},
			 MFD_TARGET_UNASSIGNED,
			 MFD_CURVE_DIP_FREE},
			{"fader_5",
			 "Track Fader 5",
			 "fader",
			 {MFD_MIDI_CC, 0U, 52U, false},
			 MFD_TARGET_UNASSIGNED,
			 MFD_CURVE_DIP_FREE},
			{"fader_6",
			 "Track Fader 6",
			 "fader",
			 {MFD_MIDI_CC, 0U, 53U, false},
			 MFD_TARGET_UNASSIGNED,
			 MFD_CURVE_DIP_FREE},
			{"fader_7",
			 "Track Fader 7",
			 "fader",
			 {MFD_MIDI_CC, 0U, 54U, false},
			 MFD_TARGET_UNASSIGNED,
			 MFD_CURVE_DIP_FREE},
			{"fader_8",
			 "Track Fader 8",
			 "fader",
			 {MFD_MIDI_CC, 0U, 55U, false},
			 MFD_TARGET_UNASSIGNED,
			 MFD_CURVE_DIP_FREE},
			{"master",
			 "Master Fader",
			 "master",
			 {MFD_MIDI_CC, 0U, 56U, false},
			 MFD_TARGET_UNASSIGNED,
			 MFD_CURVE_DIP_FREE},
			{"fx_1", "FX 1", "fx", {MFD_MIDI_NONE, 0U, 0U, false}, MFD_TARGET_UNASSIGNED, MFD_CURVE_LINEAR},
			{"fx_2", "FX 2", "fx", {MFD_MIDI_NONE, 0U, 0U, false}, MFD_TARGET_UNASSIGNED, MFD_CURVE_LINEAR},
			{"fx_3", "FX 3", "fx", {MFD_MIDI_NONE, 0U, 0U, false}, MFD_TARGET_UNASSIGNED, MFD_CURVE_LINEAR},
			{"fx_4", "FX 4", "fx", {MFD_MIDI_NONE, 0U, 0U, false}, MFD_TARGET_UNASSIGNED, MFD_CURVE_LINEAR},
			{"fx_5", "FX 5", "fx", {MFD_MIDI_NONE, 0U, 0U, false}, MFD_TARGET_UNASSIGNED, MFD_CURVE_LINEAR},
			{"fx_6", "FX 6", "fx", {MFD_MIDI_NONE, 0U, 0U, false}, MFD_TARGET_UNASSIGNED, MFD_CURVE_LINEAR},
			{"fx_7", "FX 7", "fx", {MFD_MIDI_NONE, 0U, 0U, false}, MFD_TARGET_UNASSIGNED, MFD_CURVE_LINEAR},
			{"fx_8", "FX 8", "fx", {MFD_MIDI_NONE, 0U, 0U, false}, MFD_TARGET_UNASSIGNED, MFD_CURVE_LINEAR},
		},
	},
	{
		MFD_PROFILE_AKAI_MIDIMIX,
		"midimix",
		"Akai MIDImix",
		"MIDImix",
		MFD_PROFILE_TIER_BETA_UNTESTED,
		"Beta (untested on hardware) — mapped from Akai MIDImix factory MIDI spec (Faders 1-8: Ch 1 CC 19/23/27/31/49/53/57/61, Master: Ch 1 CC 62, Bottom Knobs 1-8: Ch 1 CC 18/22/26/30/48/52/56/60).",
		false,
		8,
		true,
		8,
		MFD_DESK_UPPER_KNOB_ROWS_3,
		k_midimix_usb_tokens,
		{
			{"crossfader",
			 "Crossfader (A/B)",
			 "crossfader",
			 {MFD_MIDI_NONE, 0U, 0U, false},
			 MFD_TARGET_UNASSIGNED,
			 MFD_CURVE_DIP_FREE},
			{"fader_1",
			 "Track Fader 1",
			 "fader",
			 {MFD_MIDI_CC, 0U, 19U, false},
			 MFD_TARGET_UNASSIGNED,
			 MFD_CURVE_DIP_FREE},
			{"fader_2",
			 "Track Fader 2",
			 "fader",
			 {MFD_MIDI_CC, 0U, 23U, false},
			 MFD_TARGET_UNASSIGNED,
			 MFD_CURVE_DIP_FREE},
			{"fader_3",
			 "Track Fader 3",
			 "fader",
			 {MFD_MIDI_CC, 0U, 27U, false},
			 MFD_TARGET_UNASSIGNED,
			 MFD_CURVE_DIP_FREE},
			{"fader_4",
			 "Track Fader 4",
			 "fader",
			 {MFD_MIDI_CC, 0U, 31U, false},
			 MFD_TARGET_UNASSIGNED,
			 MFD_CURVE_DIP_FREE},
			{"fader_5",
			 "Track Fader 5",
			 "fader",
			 {MFD_MIDI_CC, 0U, 49U, false},
			 MFD_TARGET_UNASSIGNED,
			 MFD_CURVE_DIP_FREE},
			{"fader_6",
			 "Track Fader 6",
			 "fader",
			 {MFD_MIDI_CC, 0U, 53U, false},
			 MFD_TARGET_UNASSIGNED,
			 MFD_CURVE_DIP_FREE},
			{"fader_7",
			 "Track Fader 7",
			 "fader",
			 {MFD_MIDI_CC, 0U, 57U, false},
			 MFD_TARGET_UNASSIGNED,
			 MFD_CURVE_DIP_FREE},
			{"fader_8",
			 "Track Fader 8",
			 "fader",
			 {MFD_MIDI_CC, 0U, 61U, false},
			 MFD_TARGET_UNASSIGNED,
			 MFD_CURVE_DIP_FREE},
			{"master",
			 "Master Fader",
			 "master",
			 {MFD_MIDI_CC, 0U, 62U, false},
			 MFD_TARGET_UNASSIGNED,
			 MFD_CURVE_DIP_FREE},
			{"fx_1", "FX 1", "fx", {MFD_MIDI_CC, 0U, 18U, false}, MFD_TARGET_UNASSIGNED, MFD_CURVE_LINEAR},
			{"fx_2", "FX 2", "fx", {MFD_MIDI_CC, 0U, 22U, false}, MFD_TARGET_UNASSIGNED, MFD_CURVE_LINEAR},
			{"fx_3", "FX 3", "fx", {MFD_MIDI_CC, 0U, 26U, false}, MFD_TARGET_UNASSIGNED, MFD_CURVE_LINEAR},
			{"fx_4", "FX 4", "fx", {MFD_MIDI_CC, 0U, 30U, false}, MFD_TARGET_UNASSIGNED, MFD_CURVE_LINEAR},
			{"fx_5", "FX 5", "fx", {MFD_MIDI_CC, 0U, 48U, false}, MFD_TARGET_UNASSIGNED, MFD_CURVE_LINEAR},
			{"fx_6", "FX 6", "fx", {MFD_MIDI_CC, 0U, 52U, false}, MFD_TARGET_UNASSIGNED, MFD_CURVE_LINEAR},
			{"fx_7", "FX 7", "fx", {MFD_MIDI_CC, 0U, 56U, false}, MFD_TARGET_UNASSIGNED, MFD_CURVE_LINEAR},
			{"fx_8", "FX 8", "fx", {MFD_MIDI_CC, 0U, 60U, false}, MFD_TARGET_UNASSIGNED, MFD_CURVE_LINEAR},
		},
	},
	{
		MFD_PROFILE_KORG_NANOKONTROL2,
		"nanokontrol2",
		"Korg nanoKONTROL2",
		"nanoKONTROL2",
		MFD_PROFILE_TIER_BETA_UNTESTED,
		"Beta (untested on hardware) — mapped from Korg nanoKONTROL2 CC-mode factory defaults (Sliders 1-8: Ch 1 CC 0-7, Knobs 1-8: Ch 1 CC 16-23).",
		false,
		8,
		false,
		8,
		MFD_DESK_UPPER_TRANSPORT_AND_KNOBS_1,
		k_nanokontrol2_usb_tokens,
		{
			{"crossfader",
			 "Crossfader (A/B)",
			 "crossfader",
			 {MFD_MIDI_NONE, 0U, 0U, false},
			 MFD_TARGET_UNASSIGNED,
			 MFD_CURVE_DIP_FREE},
			{"fader_1",
			 "Track Fader 1",
			 "fader",
			 {MFD_MIDI_CC, 0U, 0U, false},
			 MFD_TARGET_UNASSIGNED,
			 MFD_CURVE_DIP_FREE},
			{"fader_2",
			 "Track Fader 2",
			 "fader",
			 {MFD_MIDI_CC, 0U, 1U, false},
			 MFD_TARGET_UNASSIGNED,
			 MFD_CURVE_DIP_FREE},
			{"fader_3",
			 "Track Fader 3",
			 "fader",
			 {MFD_MIDI_CC, 0U, 2U, false},
			 MFD_TARGET_UNASSIGNED,
			 MFD_CURVE_DIP_FREE},
			{"fader_4",
			 "Track Fader 4",
			 "fader",
			 {MFD_MIDI_CC, 0U, 3U, false},
			 MFD_TARGET_UNASSIGNED,
			 MFD_CURVE_DIP_FREE},
			{"fader_5",
			 "Track Fader 5",
			 "fader",
			 {MFD_MIDI_CC, 0U, 4U, false},
			 MFD_TARGET_UNASSIGNED,
			 MFD_CURVE_DIP_FREE},
			{"fader_6",
			 "Track Fader 6",
			 "fader",
			 {MFD_MIDI_CC, 0U, 5U, false},
			 MFD_TARGET_UNASSIGNED,
			 MFD_CURVE_DIP_FREE},
			{"fader_7",
			 "Track Fader 7",
			 "fader",
			 {MFD_MIDI_CC, 0U, 6U, false},
			 MFD_TARGET_UNASSIGNED,
			 MFD_CURVE_DIP_FREE},
			{"fader_8",
			 "Track Fader 8",
			 "fader",
			 {MFD_MIDI_CC, 0U, 7U, false},
			 MFD_TARGET_UNASSIGNED,
			 MFD_CURVE_DIP_FREE},
			{"master",
			 "Master Fader",
			 "master",
			 {MFD_MIDI_NONE, 0U, 0U, false},
			 MFD_TARGET_UNASSIGNED,
			 MFD_CURVE_DIP_FREE},
			{"fx_1", "FX 1", "fx", {MFD_MIDI_CC, 0U, 16U, false}, MFD_TARGET_UNASSIGNED, MFD_CURVE_LINEAR},
			{"fx_2", "FX 2", "fx", {MFD_MIDI_CC, 0U, 17U, false}, MFD_TARGET_UNASSIGNED, MFD_CURVE_LINEAR},
			{"fx_3", "FX 3", "fx", {MFD_MIDI_CC, 0U, 18U, false}, MFD_TARGET_UNASSIGNED, MFD_CURVE_LINEAR},
			{"fx_4", "FX 4", "fx", {MFD_MIDI_CC, 0U, 19U, false}, MFD_TARGET_UNASSIGNED, MFD_CURVE_LINEAR},
			{"fx_5", "FX 5", "fx", {MFD_MIDI_CC, 0U, 20U, false}, MFD_TARGET_UNASSIGNED, MFD_CURVE_LINEAR},
			{"fx_6", "FX 6", "fx", {MFD_MIDI_CC, 0U, 21U, false}, MFD_TARGET_UNASSIGNED, MFD_CURVE_LINEAR},
			{"fx_7", "FX 7", "fx", {MFD_MIDI_CC, 0U, 22U, false}, MFD_TARGET_UNASSIGNED, MFD_CURVE_LINEAR},
			{"fx_8", "FX 8", "fx", {MFD_MIDI_CC, 0U, 23U, false}, MFD_TARGET_UNASSIGNED, MFD_CURVE_LINEAR},
		},
	},
	{
		MFD_PROFILE_LAUNCH_CONTROL_XL,
		"launchcontrolxl",
		"Novation Launch Control XL",
		"Launch Control XL",
		MFD_PROFILE_TIER_BETA_UNTESTED,
		"Beta (untested on hardware) — mapped from Novation Launch Control XL User Template 1 spec (Faders 1-8: Ch 1 CC 77-84, Bottom Knobs 1-8: Ch 1 CC 49-56; use Learn if on Factory Ch 9).",
		false,
		8,
		false,
		8,
		MFD_DESK_UPPER_KNOB_ROWS_3,
		k_launchcontrolxl_usb_tokens,
		{
			{"crossfader",
			 "Crossfader (A/B)",
			 "crossfader",
			 {MFD_MIDI_NONE, 0U, 0U, false},
			 MFD_TARGET_UNASSIGNED,
			 MFD_CURVE_DIP_FREE},
			{"fader_1",
			 "Track Fader 1",
			 "fader",
			 {MFD_MIDI_CC, 0U, 77U, false},
			 MFD_TARGET_UNASSIGNED,
			 MFD_CURVE_DIP_FREE},
			{"fader_2",
			 "Track Fader 2",
			 "fader",
			 {MFD_MIDI_CC, 0U, 78U, false},
			 MFD_TARGET_UNASSIGNED,
			 MFD_CURVE_DIP_FREE},
			{"fader_3",
			 "Track Fader 3",
			 "fader",
			 {MFD_MIDI_CC, 0U, 79U, false},
			 MFD_TARGET_UNASSIGNED,
			 MFD_CURVE_DIP_FREE},
			{"fader_4",
			 "Track Fader 4",
			 "fader",
			 {MFD_MIDI_CC, 0U, 80U, false},
			 MFD_TARGET_UNASSIGNED,
			 MFD_CURVE_DIP_FREE},
			{"fader_5",
			 "Track Fader 5",
			 "fader",
			 {MFD_MIDI_CC, 0U, 81U, false},
			 MFD_TARGET_UNASSIGNED,
			 MFD_CURVE_DIP_FREE},
			{"fader_6",
			 "Track Fader 6",
			 "fader",
			 {MFD_MIDI_CC, 0U, 82U, false},
			 MFD_TARGET_UNASSIGNED,
			 MFD_CURVE_DIP_FREE},
			{"fader_7",
			 "Track Fader 7",
			 "fader",
			 {MFD_MIDI_CC, 0U, 83U, false},
			 MFD_TARGET_UNASSIGNED,
			 MFD_CURVE_DIP_FREE},
			{"fader_8",
			 "Track Fader 8",
			 "fader",
			 {MFD_MIDI_CC, 0U, 84U, false},
			 MFD_TARGET_UNASSIGNED,
			 MFD_CURVE_DIP_FREE},
			{"master",
			 "Master Fader",
			 "master",
			 {MFD_MIDI_NONE, 0U, 0U, false},
			 MFD_TARGET_UNASSIGNED,
			 MFD_CURVE_DIP_FREE},
			{"fx_1", "FX 1", "fx", {MFD_MIDI_CC, 0U, 49U, false}, MFD_TARGET_UNASSIGNED, MFD_CURVE_LINEAR},
			{"fx_2", "FX 2", "fx", {MFD_MIDI_CC, 0U, 50U, false}, MFD_TARGET_UNASSIGNED, MFD_CURVE_LINEAR},
			{"fx_3", "FX 3", "fx", {MFD_MIDI_CC, 0U, 51U, false}, MFD_TARGET_UNASSIGNED, MFD_CURVE_LINEAR},
			{"fx_4", "FX 4", "fx", {MFD_MIDI_CC, 0U, 52U, false}, MFD_TARGET_UNASSIGNED, MFD_CURVE_LINEAR},
			{"fx_5", "FX 5", "fx", {MFD_MIDI_CC, 0U, 53U, false}, MFD_TARGET_UNASSIGNED, MFD_CURVE_LINEAR},
			{"fx_6", "FX 6", "fx", {MFD_MIDI_CC, 0U, 54U, false}, MFD_TARGET_UNASSIGNED, MFD_CURVE_LINEAR},
			{"fx_7", "FX 7", "fx", {MFD_MIDI_CC, 0U, 55U, false}, MFD_TARGET_UNASSIGNED, MFD_CURVE_LINEAR},
			{"fx_8", "FX 8", "fx", {MFD_MIDI_CC, 0U, 56U, false}, MFD_TARGET_UNASSIGNED, MFD_CURVE_LINEAR},
		},
	},
};

#define MFD_PROFILE_REGISTRY_COUNT (sizeof(k_profile_registry) / sizeof(k_profile_registry[0]))

static bool contains_case_insensitive(const char *haystack, const char *needle)
{
	if (!haystack || !needle || !*needle) {
		return false;
	}
	size_t nlen = strlen(needle);
	for (const char *p = haystack; *p; ++p) {
		size_t i = 0;
		while (i < nlen && p[i] && tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) {
			++i;
		}
		if (i == nlen) {
			return true;
		}
	}
	return false;
}

static bool equals_case_insensitive(const char *a, const char *b)
{
	if (!a || !b) {
		return false;
	}
	while (*a && *b) {
		if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) {
			return false;
		}
		++a;
		++b;
	}
	return (*a == '\0' && *b == '\0');
}

size_t mfd_profile_count(void)
{
	return MFD_PROFILE_REGISTRY_COUNT;
}

const struct mfd_controller_profile_def *mfd_profile_get_by_index(size_t index)
{
	if (index >= MFD_PROFILE_REGISTRY_COUNT) {
		return NULL;
	}
	return &k_profile_registry[index];
}

const struct mfd_controller_profile_def *mfd_profile_get_def(enum mfd_controller_profile profile)
{
	for (size_t i = 0U; i < MFD_PROFILE_REGISTRY_COUNT; ++i) {
		if (k_profile_registry[i].id == profile) {
			return &k_profile_registry[i];
		}
	}
	return &k_profile_registry[0];
}

const struct mfd_controller_profile_def *mfd_profile_find_by_slug(const char *slug)
{
	if (!slug || !*slug) {
		return NULL;
	}
	for (size_t i = 0U; i < MFD_PROFILE_REGISTRY_COUNT; ++i) {
		if (equals_case_insensitive(k_profile_registry[i].slug, slug)) {
			return &k_profile_registry[i];
		}
	}
	return NULL;
}

const char *mfd_profile_tier_to_string(enum mfd_profile_support_tier tier)
{
	switch (tier) {
	case MFD_PROFILE_TIER_VERIFIED:
		return "verified";
	case MFD_PROFILE_TIER_BETA_UNTESTED:
		return "beta";
	case MFD_PROFILE_TIER_CUSTOM:
	default:
		return "custom";
	}
}

bool mfd_slot_is_fx_bank(int slot_index)
{
	return slot_index >= (int)MFD_FADER_STRIP_COUNT && slot_index < (int)MFD_APC40_DEFAULT_SLOT_COUNT;
}

bool mfd_profile_has_physical_slot(enum mfd_controller_profile profile, int slot_index)
{
	if (slot_index < 0 || (size_t)slot_index >= MFD_APC40_DEFAULT_SLOT_COUNT) {
		return false;
	}
	const struct mfd_controller_profile_def *def = mfd_profile_get_def(profile);
	if (!def) {
		return true;
	}
	if (slot_index == 0) {
		return def->has_crossfader;
	}
	if (slot_index >= 1 && slot_index <= 8) {
		return slot_index <= def->track_fader_count;
	}
	if (slot_index == 9) {
		return def->has_master_fader;
	}
	return (slot_index - (int)MFD_FADER_STRIP_COUNT) < def->fx_knob_count;
}

enum mfd_controller_profile mfd_profile_match_device_name(const char *device_name)
{
	if (!device_name || !*device_name) {
		return MFD_PROFILE_GENERIC;
	}
	for (size_t i = 0U; i < MFD_PROFILE_REGISTRY_COUNT; ++i) {
		const struct mfd_controller_profile_def *def = &k_profile_registry[i];
		if (!def->usb_match_tokens) {
			continue;
		}
		for (size_t t = 0U; def->usb_match_tokens[t] != NULL; ++t) {
			if (contains_case_insensitive(device_name, def->usb_match_tokens[t])) {
				return def->id;
			}
		}
	}
	return MFD_PROFILE_GENERIC;
}

bool mfd_is_apc40_mk2_name(const char *device_name)
{
	return mfd_profile_match_device_name(device_name) == MFD_PROFILE_APC40_MK2;
}

enum mfd_controller_profile mfd_profile_for_device(const char *configured_name, bool is_open, const char *opened_name)
{
	if (configured_name && configured_name[0] != '\0') {
		return mfd_profile_match_device_name(configured_name);
	}
	if (is_open && opened_name && opened_name[0] != '\0') {
		return mfd_profile_match_device_name(opened_name);
	}
	return MFD_PROFILE_GENERIC;
}

size_t mfd_apc40_get_default_slots(struct mfd_apc40_default_slot *out_slots, size_t max_slots)
{
	if (!out_slots || max_slots == 0U) {
		return MFD_APC40_DEFAULT_SLOT_COUNT;
	}
	const struct mfd_controller_profile_def *apc = mfd_profile_get_def(MFD_PROFILE_APC40_MK2);
	size_t count = (max_slots < MFD_APC40_DEFAULT_SLOT_COUNT) ? max_slots : MFD_APC40_DEFAULT_SLOT_COUNT;
	memcpy(out_slots, apc->slots, count * sizeof(struct mfd_apc40_default_slot));
	return count;
}

int mfd_slot_index_for_id(const char *slot_id)
{
	if (!slot_id || !*slot_id) {
		return -1;
	}
	const struct mfd_controller_profile_def *apc = mfd_profile_get_def(MFD_PROFILE_APC40_MK2);
	for (size_t i = 0U; i < MFD_APC40_DEFAULT_SLOT_COUNT; ++i) {
		if (strcmp(apc->slots[i].id, slot_id) == 0) {
			return (int)i;
		}
	}
	return -1;
}

const char *mfd_slot_id_for_index(int index)
{
	if (index < 0 || (size_t)index >= MFD_APC40_DEFAULT_SLOT_COUNT) {
		return "";
	}
	const struct mfd_controller_profile_def *apc = mfd_profile_get_def(MFD_PROFILE_APC40_MK2);
	return apc->slots[index].id;
}

const char *mfd_slot_kind_for_index(int index)
{
	if (index < 0 || (size_t)index >= MFD_APC40_DEFAULT_SLOT_COUNT) {
		return "fader";
	}
	const struct mfd_controller_profile_def *apc = mfd_profile_get_def(MFD_PROFILE_APC40_MK2);
	return apc->slots[index].kind;
}

const char *mfd_target_mode_to_string(enum mfd_target_type target)
{
	switch (target) {
	case MFD_TARGET_TBAR:
		return "tbar";
	case MFD_TARGET_TWO_SOURCE_MIX:
		return "crossfade_ab";
	case MFD_TARGET_SINGLE_LAYER:
		return "layer_opacity";
	case MFD_TARGET_AUDIO_VOLUME:
		return "audio_volume";
	case MFD_TARGET_FILTER_PARAM:
		return "filter_param";
	case MFD_TARGET_UNASSIGNED:
	default:
		return "unassigned";
	}
}

bool mfd_target_mode_from_string(const char *str, enum mfd_target_type *out_target)
{
	if (!str || !out_target) {
		return false;
	}
	if (strcmp(str, "unassigned") == 0) {
		*out_target = MFD_TARGET_UNASSIGNED;
		return true;
	}
	if (strcmp(str, "tbar") == 0) {
		*out_target = MFD_TARGET_TBAR;
		return true;
	}
	if (strcmp(str, "crossfade_ab") == 0) {
		*out_target = MFD_TARGET_TWO_SOURCE_MIX;
		return true;
	}
	if (strcmp(str, "layer_opacity") == 0) {
		*out_target = MFD_TARGET_SINGLE_LAYER;
		return true;
	}
	if (strcmp(str, "audio_volume") == 0) {
		*out_target = MFD_TARGET_AUDIO_VOLUME;
		return true;
	}
	if (strcmp(str, "filter_param") == 0) {
		*out_target = MFD_TARGET_FILTER_PARAM;
		return true;
	}
	return false;
}

const char *mfd_curve_to_string(enum mfd_crossfade_curve curve)
{
	switch (curve) {
	case MFD_CURVE_DIP_FREE:
		return "dip_free";
	case MFD_CURVE_FULL_CENTER:
		return "full_center";
	case MFD_CURVE_COSINE:
		return "equal_power";
	case MFD_CURVE_LINEAR:
		return "linear";
	case MFD_CURVE_CUT:
		return "cut";
	default:
		return "dip_free";
	}
}

bool mfd_curve_from_string(const char *str, enum mfd_crossfade_curve *out_curve)
{
	if (!str || !out_curve) {
		return false;
	}
	if (strcmp(str, "dip_free") == 0) {
		*out_curve = MFD_CURVE_DIP_FREE;
		return true;
	}
	if (strcmp(str, "full_center") == 0) {
		*out_curve = MFD_CURVE_FULL_CENTER;
		return true;
	}
	if (strcmp(str, "equal_power") == 0) {
		*out_curve = MFD_CURVE_COSINE;
		return true;
	}
	if (strcmp(str, "linear") == 0) {
		*out_curve = MFD_CURVE_LINEAR;
		return true;
	}
	if (strcmp(str, "cut") == 0) {
		*out_curve = MFD_CURVE_CUT;
		return true;
	}
	return false;
}

void mfd_row_controls_for_target(enum mfd_target_type target, bool *out_show_source_a, bool *out_show_source_b,
				 bool *out_show_curve, bool *out_show_afv)
{
	bool show_a = false;
	bool show_b = false;
	bool show_curve = false;
	bool show_afv = false;

	switch (target) {
	case MFD_TARGET_TWO_SOURCE_MIX:
		show_a = true;
		show_b = true;
		show_curve = true;
		show_afv = true;
		break;
	case MFD_TARGET_SINGLE_LAYER:
		show_a = true;
		show_b = false;
		show_curve = false;
		show_afv = true;
		break;
	case MFD_TARGET_AUDIO_VOLUME:
	case MFD_TARGET_FILTER_PARAM:
		show_a = true;
		show_b = false;
		show_curve = false;
		show_afv = false;
		break;
	case MFD_TARGET_TBAR:
	case MFD_TARGET_UNASSIGNED:
	default:
		break;
	}

	if (out_show_source_a) {
		*out_show_source_a = show_a;
	}
	if (out_show_source_b) {
		*out_show_source_b = show_b;
	}
	if (out_show_curve) {
		*out_show_curve = show_curve;
	}
	if (out_show_afv) {
		*out_show_afv = show_afv;
	}
}
