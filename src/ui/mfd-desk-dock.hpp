/*
 * Midi Fadester
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

class QWidget;
struct mfd_engine;

/* The Fadester Desk dock (0.8.0): the APC40 mkII on screen — scene buttons, the pad grid in the desk's own colours,
 * Clip Stop, the faders — and Visual Sidechain's tempo, beat, lock and panic. Its buttons act like the desk's. Qt (UI)
 * thread only. */

/* Registers the dock with OBS (module load, before OBS restores the dock layout). */
void mfd_desk_dock_init(void);

/* Repaints the dock when it is on screen: the UI dispatcher calls it after the engine asks (no timers: the engine's
 * frame tick asks about 30 times a second while the dock is visible). */
void mfd_desk_dock_refresh(void);

/* Shows the dock (the settings dialog's button, and once by itself the first time an APC40 mkII is seen): false when it
 * is not registered (no OBS frontend). UI thread. */
bool mfd_desk_dock_show(void);

/* What the dock's gear button opens: the settings dialog registers its opener here (nullptr takes it away). */
void mfd_desk_dock_set_settings_opener(void (*open_settings)(void));

/* Takes the dock out again (idempotent; OBS_FRONTEND_EVENT_EXIT does it first when OBS quits). */
void mfd_desk_dock_shutdown(void);

/* A dock widget on this engine, not registered with OBS (the UI test renders it). */
QWidget *mfd_desk_dock_create(struct mfd_engine *eng, QWidget *parent = nullptr);
