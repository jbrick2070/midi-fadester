/*
 * Midi Fadester
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "ui/mfd-controller-svg.hpp"
#include "ui/mfd-svg-canvas.hpp"
#include "core/ui-sentences.h"
#include "core/mixer-math.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QString>
#include <QVBoxLayout>

#include <cmath>

namespace {

static QString xml_escape(const QString &in)
{
	QString out;
	out.reserve(in.size() + 8);
	for (QChar ch : in) {
		switch (ch.unicode()) {
		case '&':
			out.append(QStringLiteral("&amp;"));
			break;
		case '<':
			out.append(QStringLiteral("&lt;"));
			break;
		case '>':
			out.append(QStringLiteral("&gt;"));
			break;
		case '"':
			out.append(QStringLiteral("&quot;"));
			break;
		case '\'':
			out.append(QStringLiteral("&apos;"));
			break;
		default:
			out.append(ch);
			break;
		}
	}
	return out;
}

static QString elide_plain(const QString &s, int max_chars)
{
	if (s.size() <= max_chars) {
		return s;
	}
	return s.left(max_chars - 1) + QStringLiteral("\xE2\x80\xA6");
}

} // namespace

QByteArray mfd_make_mode_card_svg(enum mfd_target_type mode)
{
	switch (mode) {
	case MFD_TARGET_TBAR:
		return QByteArrayLiteral(
			"<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 180 68'>"
			"<rect width='180' height='68' rx='8' fill='#0f172a'/>"
			"<rect x='12' y='12' width='56' height='36' rx='4' fill='#1e3a8a' stroke='#60a5fa' stroke-width='1.5'/>"
			"<text x='40' y='34' fill='#bfdbfe' font-family='Segoe UI, sans-serif' font-size='10' font-weight='bold' text-anchor='middle'>Preview</text>"
			"<path d='M75 30 L103 30 M97 24 L104 30 L97 36' stroke='#38bdf8' stroke-width='2.2' fill='none' stroke-linecap='round' stroke-linejoin='round'/>"
			"<rect x='112' y='12' width='56' height='36' rx='4' fill='#7f1d1d' stroke='#f87171' stroke-width='1.5'/>"
			"<text x='140' y='34' fill='#fecaca' font-family='Segoe UI, sans-serif' font-size='10' font-weight='bold' text-anchor='middle'>Program</text>"
			"<rect x='46' y='55' width='88' height='4' rx='2' fill='#334155'/>"
			"<rect x='96' y='51' width='12' height='12' rx='3' fill='#38bdf8'/>"
			"</svg>");

	case MFD_TARGET_TWO_SOURCE_MIX:
		return QByteArrayLiteral(
			"<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 180 68'>"
			"<rect width='180' height='68' rx='8' fill='#0f172a'/>"
			"<rect x='22' y='10' width='76' height='40' rx='5' fill='#1d4ed8' stroke='#60a5fa' stroke-width='1.5'/>"
			"<text x='48' y='33' fill='#dbeafe' font-family='Segoe UI, sans-serif' font-size='10' font-weight='bold'>Left</text>"
			"<rect x='82' y='16' width='76' height='40' rx='5' fill='#047857' fill-opacity='0.82' stroke='#34d399' stroke-width='1.5'/>"
			"<text x='124' y='40' fill='#d1fae5' font-family='Segoe UI, sans-serif' font-size='10' font-weight='bold' text-anchor='middle'>Right</text>"
			"</svg>");

	case MFD_TARGET_SINGLE_LAYER:
		return QByteArrayLiteral(
			"<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 180 68'>"
			"<rect width='180' height='68' rx='8' fill='#0f172a'/>"
			"<rect x='24' y='10' width='132' height='48' rx='5' fill='#1e293b' stroke='#475569' stroke-width='1.2'/>"
			"<rect x='50' y='18' width='80' height='32' rx='4' fill='#7c3aed' fill-opacity='0.65' stroke='#c4b5fd' stroke-width='1.5' stroke-dasharray='4 2'/>"
			"<text x='90' y='38' fill='#ede9fe' font-family='Segoe UI, sans-serif' font-size='10' font-weight='bold' text-anchor='middle'>Layer 0-100%</text>"
			"</svg>");

	case MFD_TARGET_AUDIO_VOLUME:
	default:
		return QByteArrayLiteral(
			"<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 180 68'>"
			"<rect width='180' height='68' rx='8' fill='#0f172a'/>"
			"<rect x='36' y='40' width='12' height='16' rx='2' fill='#22c55e'/>"
			"<rect x='56' y='32' width='12' height='24' rx='2' fill='#22c55e'/>"
			"<rect x='76' y='23' width='12' height='33' rx='2' fill='#eab308'/>"
			"<rect x='96' y='14' width='12' height='42' rx='2' fill='#38bdf8'/>"
			"<rect x='132' y='12' width='6' height='44' rx='3' fill='#334155'/>"
			"<rect x='126' y='20' width='18' height='10' rx='2' fill='#f8fafc' stroke='#38bdf8' stroke-width='1.5'/>"
			"</svg>");
	}
}

QByteArray mfd_make_apc40_desk_svg(const struct mfd_ui_snapshot &snap)
{
	struct mfd_ui_controller_presentation ctrl;
	mfd_ui_format_controller_status(snap.midi_connected, snap.configured_device_name, snap.opened_device_name,
					snap.midi_error_kind, &ctrl);

	QString svg;
	svg.reserve(16384);
	svg.append(QStringLiteral(
		"<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 980 500'>"
		"<rect width='980' height='500' rx='16' fill='#0b1120' stroke='#334155' stroke-width='2'/>"
		"<rect x='18' y='16' width='944' height='468' rx='12' fill='#111827' stroke='#1f2937' stroke-width='1.5'/>"));

	/* Desk header */
	const QString dot_hex = ctrl.dot_green ? QStringLiteral("#22c55e") : QStringLiteral("#ef4444");
	svg.append(QStringLiteral("<text x='38' y='48' fill='#f8fafc' font-family='Segoe UI, sans-serif' "
				  "font-size='18' font-weight='bold'>Akai APC40 mkII \xE2\x80\x94 Hardware Map</text>"
				  "<circle cx='355' cy='42' r='6' fill='%1'/>"
				  "<text x='370' y='47' fill='#cbd5e1' font-family='Segoe UI, sans-serif' "
				  "font-size='13'>%2</text>")
			   .arg(dot_hex, xml_escape(QString::fromUtf8(ctrl.status_sentence))));

	/* Decorative 8x4 clip launch grid silhouette for instant physical desk orientation */
	for (int col = 0; col < 8; ++col) {
		const int gx = 42 + col * 84;
		for (int row = 0; row < 3; ++row) {
			const int gy = 68 + row * 22;
			svg.append(QStringLiteral("<rect x='%1' y='%2' width='66' height='15' rx='3' "
						  "fill='#1e293b' stroke='#334155' stroke-width='1'/>")
					   .arg(gx)
					   .arg(gy));
		}
	}
	/* Scene launch column silhouette above Master */
	for (int row = 0; row < 3; ++row) {
		const int gy = 68 + row * 22;
		svg.append(QStringLiteral("<rect x='740' y='%1' width='76' height='15' rx='3' "
					  "fill='#1e293b' stroke='#475569' stroke-width='1'/>")
				   .arg(gy));
	}

	auto accent_for_strip = [](const struct mfd_ui_strip_presentation &sp) -> QString {
		if (sp.dimmed) {
			return QStringLiteral("#475569");
		}
		if (sp.severity == MFD_UI_SEVERITY_RED) {
			return QStringLiteral("#ef4444");
		}
		if (sp.severity == MFD_UI_SEVERITY_AMBER) {
			return QStringLiteral("#f59e0b");
		}
		if (std::strcmp(sp.state_readout, "At end.") == 0) {
			return QStringLiteral("#10b981");
		}
		return QStringLiteral("#38bdf8");
	};

	auto build_strip_pres = [&](int idx, struct mfd_ui_strip_presentation *out) {
		const struct mfd_fader_slot &s = snap.slots[idx];
		struct mfd_ui_strip_input in;
		std::memset(&in, 0, sizeof(in));
		in.slot_index = idx;
		in.slot_label = s.label;
		in.is_horizontal = (idx == 0);
		in.enabled = s.enabled;
		in.addr_type = s.addr.type;
		in.target_type = s.target_type;
		in.scene_name = (s.scene_name[0] != '\0') ? s.scene_name : s.resolved_scene_name;
		in.source_a_name = s.source_a_name;
		in.source_b_name = s.source_b_name;
		in.available = s.cached_available;
		in.reason = s.cached_reason;
		in.raw_norm = s.last_raw_norm;
		in.applied_val = s.last_applied_val;
		in.alpha_a = s.last_alpha_a;
		in.alpha_b = s.last_alpha_b;
		in.audio_db = mfd_deflection_to_db(s.last_applied_val);
		in.auto_fade_active = s.auto_fade_active;
		in.tbar_state = s.tbar_state;
		in.takeover_state = s.takeover.state;
		in.takeover_hint = s.takeover.hint;
		in.has_last_fader = s.takeover.has_last_fader;
		in.polarity_reversed = s.takeover.polarity_reversed;
		in.target_obs = s.takeover.target_obs;
		mfd_ui_format_strip_presentation(&in, out);
	};

	/* Vertical faders: Track 1..8 (indices 1..8) + Master (index 9) */
	for (int v = 0; v < 9; ++v) {
		const int slot_idx = v + 1;
		const int cx = (v < 8) ? (75 + v * 84) : 778;
		struct mfd_ui_strip_presentation sp;
		build_strip_pres(slot_idx, &sp);
		const QString accent = accent_for_strip(sp);
		const QString title = xml_escape(elide_plain(QString::fromUtf8(sp.title), 10));
		const QString short_lbl = xml_escape(elide_plain(QString::fromUtf8(sp.svg_short_label), 13));

		const int track_top = 172;
		const int track_h = 160;
		const int fill_h = (sp.bar_percent * track_h) / 100;
		const int cap_y = track_top + track_h - fill_h - 8;

		svg.append(QStringLiteral("<rect x='%1' y='146' width='74' height='248' rx='7' fill='#0f172a' "
					  "stroke='%2' stroke-width='1.5'/>"
					  "<text x='%3' y='164' fill='#f8fafc' font-family='Segoe UI, sans-serif' "
					  "font-size='12' font-weight='bold' text-anchor='middle'>%4</text>"
					  "<rect x='%5' y='%6' width='8' height='%7' rx='4' fill='#1e293b'/>"
					  "<rect x='%5' y='%8' width='8' height='%9' rx='4' fill='%2'/>"
					  "<rect x='%10' y='%11' width='26' height='16' rx='3' fill='#e2e8f0' "
					  "stroke='%2' stroke-width='1.5'/>"
					  "<text x='%3' y='356' fill='#cbd5e1' font-family='Segoe UI, sans-serif' "
					  "font-size='10' text-anchor='middle'>%12%</text>"
					  "<text x='%3' y='378' fill='%2' font-family='Segoe UI, sans-serif' "
					  "font-size='10' font-weight='bold' text-anchor='middle'>%13</text>")
				   .arg(cx - 37)
				   .arg(accent)
				   .arg(cx)
				   .arg(title)
				   .arg(cx - 4)
				   .arg(track_top)
				   .arg(track_h)
				   .arg(track_top + track_h - fill_h)
				   .arg(fill_h)
				   .arg(cx - 13)
				   .arg(cap_y)
				   .arg(sp.bar_percent)
				   .arg(short_lbl));
	}

	/* Horizontal Crossfader (index 0) at bottom right of APC40 mkII */
	{
		struct mfd_ui_strip_presentation sp0;
		build_strip_pres(0, &sp0);
		const QString accent0 = accent_for_strip(sp0);
		const QString assign0 = xml_escape(elide_plain(QString::fromUtf8(sp0.assignment_sentence), 36));
		const int xf_left = 680;
		const int xf_w = 190;
		const int fill_w = (sp0.bar_percent * xf_w) / 100;
		const int cap_x = xf_left + fill_w - 8;

		svg.append(QStringLiteral("<rect x='642' y='406' width='298' height='68' rx='8' fill='#0f172a' "
					  "stroke='%1' stroke-width='1.8'/>"
					  "<text x='658' y='426' fill='#f8fafc' font-family='Segoe UI, sans-serif' "
					  "font-size='12' font-weight='bold'>%2 (%3%)</text>"
					  "<text x='924' y='426' fill='%1' font-family='Segoe UI, sans-serif' "
					  "font-size='11' font-weight='bold' text-anchor='end'>%4</text>"
					  "<rect x='%5' y='442' width='%6' height='8' rx='4' fill='#1e293b'/>"
					  "<rect x='%5' y='442' width='%7' height='8' rx='4' fill='%1'/>"
					  "<rect x='%8' y='434' width='16' height='24' rx='3' fill='#f8fafc' "
					  "stroke='%1' stroke-width='1.5'/>")
				   .arg(accent0)
				   .arg(xml_escape(elide_plain(QString::fromUtf8(sp0.title), 18)))
				   .arg(sp0.bar_percent)
				   .arg(assign0)
				   .arg(xf_left)
				   .arg(xf_w)
				   .arg(fill_w)
				   .arg(cap_x));
	}

	/* Legend at bottom left */
	svg.append(QStringLiteral(
		"<circle cx='52' cy='442' r='5' fill='#38bdf8'/>"
		"<text x='64' y='446' fill='#cbd5e1' font-family='Segoe UI, sans-serif' font-size='11'>Active / Ready</text>"
		"<circle cx='168' cy='442' r='5' fill='#10b981'/>"
		"<text x='180' y='446' fill='#cbd5e1' font-family='Segoe UI, sans-serif' font-size='11'>At end (parked)</text>"
		"<circle cx='294' cy='442' r='5' fill='#f59e0b'/>"
		"<text x='306' y='446' fill='#cbd5e1' font-family='Segoe UI, sans-serif' font-size='11'>Needs pickup / Studio Mode</text>"
		"<circle cx='484' cy='442' r='5' fill='#ef4444'/>"
		"<text x='496' y='446' fill='#cbd5e1' font-family='Segoe UI, sans-serif' font-size='11'>Needs attention</text>"
		"</svg>"));

	return svg.toUtf8();
}

MfdControllerViewDialog::MfdControllerViewDialog(QWidget *parent) : QDialog(parent)
{
	setObjectName(QStringLiteral("mfdControllerSvgDialog"));
	setWindowTitle(QStringLiteral("Midi Fadester \xE2\x80\x94 Controller Map"));
	resize(960, 560);
	setStyleSheet(QStringLiteral("QDialog { background-color: #0b1120; color: #f8fafc; }"));

	auto *root = new QVBoxLayout(this);
	root->setContentsMargins(14, 14, 14, 14);
	root->setSpacing(10);

	subtitle_label_ = new QLabel(
		QStringLiteral("Move any physical fader to verify its live position and assignment on the desk."),
		this);
	subtitle_label_->setObjectName(QStringLiteral("mfdControllerSvgSubtitle"));
	subtitle_label_->setStyleSheet(QStringLiteral("color: #94a3b8; font-size: 13px;"));
	root->addWidget(subtitle_label_);

	svg_canvas_ = new MfdSvgCanvas(this);
	svg_canvas_->setObjectName(QStringLiteral("mfdControllerSvgCanvas"));
	svg_canvas_->setMinimumSize(860, 440);
	root->addWidget(svg_canvas_, 1);

	auto *footer = new QHBoxLayout();
	footer->addStretch(1);
	auto *btn_close = new QPushButton(QStringLiteral("Close"), this);
	btn_close->setObjectName(QStringLiteral("mfdControllerSvgCloseBtn"));
	btn_close->setStyleSheet(
		QStringLiteral("QPushButton { background-color: #1e293b; color: #f8fafc; border: 1px solid #475569; "
			       "border-radius: 5px; padding: 6px 18px; font-weight: bold; }"));
	connect(btn_close, &QPushButton::clicked, this, &QDialog::close);
	footer->addWidget(btn_close);
	root->addLayout(footer);
}

void MfdControllerViewDialog::refresh_from_snapshot(const struct mfd_ui_snapshot &snap)
{
	if (svg_canvas_) {
		svg_canvas_->set_svg_utf8(mfd_make_apc40_desk_svg(snap));
	}
}
