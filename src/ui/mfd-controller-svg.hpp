/*
 * Midi Fadester
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include "obs/mfd-engine.h"
#include <QByteArray>
#include <QDialog>

class MfdSvgCanvas;
class QLabel;

struct mfd_ui_strip_presentation;
void mfd_ui_strip_presentation_from_slot(int idx, const struct mfd_fader_slot &s,
					 struct mfd_ui_strip_presentation *out);
QByteArray mfd_make_mode_card_svg(enum mfd_target_type mode);
QByteArray mfd_make_controller_desk_svg(const struct mfd_ui_snapshot &snap);
QByteArray mfd_make_apc40_desk_svg(const struct mfd_ui_snapshot &snap);

class MfdControllerViewDialog final : public QDialog {
public:
	explicit MfdControllerViewDialog(QWidget *parent = nullptr);
	void refresh_from_snapshot(const struct mfd_ui_snapshot &snap);

private:
	QLabel *subtitle_label_ = nullptr;
	MfdSvgCanvas *svg_canvas_ = nullptr;
};
