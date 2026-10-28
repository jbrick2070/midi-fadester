/*
 * Midi Fadester
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void mfd_ui_init(void);
void mfd_ui_shutdown(void);

#ifdef __cplusplus
}

class QDialog;
class QWidget;
struct mfd_engine;

QDialog *mfd_ui_create_settings_dialog(struct mfd_engine *eng, QWidget *parent = nullptr);
#endif
