/*
 * Midi Fadester
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "ui/mfd-controller-svg.hpp"
#include "ui/mfd-svg-canvas.hpp"
#include "core/ui-sentences.h"
#include "core/mixer-math.h"
#include "ui/mfd-text-qt.hpp"

#include <QFont>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QString>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <cstring>

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

/* Fits a text into max_px at the SVG's font, by measured width, not by character count (CJK and long words). */
static QString elide_px(const QString &s, int font_px, bool bold, int max_px)
{
	QFont font(QStringLiteral("Segoe UI"));
	font.setPixelSize(font_px);
	font.setBold(bold);
	return QFontMetrics(font).elidedText(s, Qt::ElideRight, max_px);
}

static int text_px(const QString &s, int font_px, bool bold)
{
	QFont font(QStringLiteral("Segoe UI"));
	font.setPixelSize(font_px);
	font.setBold(bold);
	return QFontMetrics(font).horizontalAdvance(s);
}

/* One SVG <text>: attrs hold only geometry and colours, the text is plain locale text escaped here, last. */
static QString svg_text(const QString &attrs, const QString &text)
{
	return QStringLiteral("<text ") + attrs + QStringLiteral(">") + xml_escape(text) + QStringLiteral("</text>");
}

static QString compact_midi_addr(const struct mfd_midi_addr &addr)
{
	const QString ch = QString::number(static_cast<int>(addr.channel) + 1);
	const QString num = QString::number(static_cast<int>(addr.number));
	switch (addr.type) {
	case MFD_MIDI_CC:
		return mfdFill(mfdText("Map.Addr.Cc", "Ch %1 CC %2"), {ch, num});
	case MFD_MIDI_PITCH_BEND:
		return mfdFill(mfdText("Map.Addr.Bend", "Ch %1 Bend"), {ch});
	case MFD_MIDI_NOTE_ON:
		return mfdFill(mfdText("Map.Addr.Note", "Ch %1 Note %2"), {ch, num});
	default:
		return mfdText("Map.Addr.Unbound", "Not assigned");
	}
}

static const char k_font[] = "font-family='Segoe UI, sans-serif'";

} // namespace

QByteArray mfd_make_mode_card_svg(enum mfd_target_type mode)
{
	const QString font = QString::fromLatin1(k_font);
	QString svg = QStringLiteral("<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 180 68'>"
				     "<rect width='180' height='68' rx='8' fill='#0f172a'/>");
	switch (mode) {
	case MFD_TARGET_TBAR:
		svg += QStringLiteral(
			"<rect x='12' y='12' width='56' height='36' rx='4' fill='#1e3a8a' stroke='#60a5fa' stroke-width='1.5'/>");
		svg += svg_text(QStringLiteral("x='40' y='34' fill='#bfdbfe' %1 font-size='10' font-weight='bold' "
					       "text-anchor='middle'")
					.arg(font),
				elide_px(mfdText("Map.ModePreview", "Preview"), 10, true, 54));
		svg += QStringLiteral(
			"<path d='M75 30 L103 30 M97 24 L104 30 L97 36' stroke='#38bdf8' stroke-width='2.2' fill='none' "
			"stroke-linecap='round' stroke-linejoin='round'/>"
			"<rect x='112' y='12' width='56' height='36' rx='4' fill='#7f1d1d' stroke='#f87171' stroke-width='1.5'/>");
		svg += svg_text(QStringLiteral("x='140' y='34' fill='#fecaca' %1 font-size='10' font-weight='bold' "
					       "text-anchor='middle'")
					.arg(font),
				elide_px(mfdText("Map.ModeProgram", "Program"), 10, true, 54));
		svg += QStringLiteral("<rect x='46' y='55' width='88' height='4' rx='2' fill='#334155'/>"
				      "<rect x='96' y='51' width='12' height='12' rx='3' fill='#38bdf8'/>");
		break;

	case MFD_TARGET_TWO_SOURCE_MIX:
		svg += QStringLiteral(
			"<rect x='22' y='10' width='76' height='40' rx='5' fill='#1d4ed8' stroke='#60a5fa' stroke-width='1.5'/>");
		svg += svg_text(
			QStringLiteral("x='48' y='33' fill='#dbeafe' %1 font-size='10' font-weight='bold'").arg(font),
			elide_px(mfdText("Map.ModeLeft", "Left"), 10, true, 48));
		svg += QStringLiteral(
			"<rect x='82' y='16' width='76' height='40' rx='5' fill='#047857' fill-opacity='0.82' "
			"stroke='#34d399' stroke-width='1.5'/>");
		svg += svg_text(QStringLiteral("x='124' y='40' fill='#d1fae5' %1 font-size='10' font-weight='bold' "
					       "text-anchor='middle'")
					.arg(font),
				elide_px(mfdText("Map.ModeRight", "Right"), 10, true, 72));
		break;

	case MFD_TARGET_SINGLE_LAYER:
		svg += QStringLiteral(
			"<rect x='24' y='10' width='132' height='48' rx='5' fill='#1e293b' stroke='#475569' stroke-width='1.2'/>"
			"<rect x='50' y='18' width='80' height='32' rx='4' fill='#7c3aed' fill-opacity='0.65' stroke='#c4b5fd' "
			"stroke-width='1.5' stroke-dasharray='4 2'/>");
		svg += svg_text(QStringLiteral("x='90' y='38' fill='#ede9fe' %1 font-size='10' font-weight='bold' "
					       "text-anchor='middle'")
					.arg(font),
				elide_px(mfdText("Map.ModeLayer", "Layer 0-100%"), 10, true, 128));
		break;

	case MFD_TARGET_FILTER_PARAM:
		svg += QStringLiteral(
			"<circle cx='54' cy='34' r='20' fill='#083344' stroke='#06b6d4' stroke-width='2.2'/>"
			"<path d='M54 34 L67 21' stroke='#22d3ee' stroke-width='2.4' stroke-linecap='round'/>"
			"<rect x='90' y='17' width='68' height='8' rx='4' fill='#1e293b'/>"
			"<rect x='90' y='17' width='48' height='8' rx='4' fill='#06b6d4'/>"
			"<rect x='90' y='31' width='68' height='8' rx='4' fill='#1e293b'/>"
			"<rect x='90' y='31' width='32' height='8' rx='4' fill='#f59e0b'/>");
		svg += svg_text(QStringLiteral("x='124' y='54' fill='#cffafe' %1 font-size='9.5' font-weight='bold' "
					       "text-anchor='middle'")
					.arg(font),
				elide_px(mfdText("Map.ModeFilter", "FX / Filter"), 10, true, 100));
		break;

	case MFD_TARGET_MIX_MASTER:
	case MFD_TARGET_AUDIO_VOLUME:
	default:
		svg += QStringLiteral(
			"<rect x='36' y='40' width='12' height='16' rx='2' fill='#22c55e'/>"
			"<rect x='56' y='32' width='12' height='24' rx='2' fill='#22c55e'/>"
			"<rect x='76' y='23' width='12' height='33' rx='2' fill='#eab308'/>"
			"<rect x='96' y='14' width='12' height='42' rx='2' fill='#38bdf8'/>"
			"<rect x='132' y='12' width='6' height='44' rx='3' fill='#334155'/>"
			"<rect x='126' y='20' width='18' height='10' rx='2' fill='#f8fafc' stroke='#38bdf8' stroke-width='1.5'/>");
		break;
	}
	svg += QStringLiteral("</svg>");
	return svg.toUtf8();
}

QByteArray mfd_make_controller_desk_svg(const struct mfd_ui_snapshot &snap)
{
	const struct mfd_controller_profile_def *def = mfd_profile_get_def(snap.active_profile);
	if (!def) {
		def = mfd_profile_get_def(MFD_PROFILE_APC40_MK2);
	}
	const bool is_beta = (def->tier == MFD_PROFILE_TIER_BETA_UNTESTED);
	const QString font = QString::fromLatin1(k_font);

	struct mfd_ui_controller_presentation ctrl;
	mfd_ui_format_controller_status(snap.midi_connected, snap.configured_device_name, snap.opened_device_name,
					snap.midi_error_kind, &ctrl);

	QString svg;
	svg.reserve(20480);
	svg.append(QStringLiteral(
		"<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 980 500'>"
		"<rect width='980' height='500' rx='16' fill='#0b1120' stroke='#334155' stroke-width='2'/>"
		"<rect x='18' y='16' width='944' height='468' rx='12' fill='#111827' stroke='#1f2937' stroke-width='1.5'/>"));

	/* Desk header: the status sits after the measured title; the Beta badge sizes to its text */
	QString badge_txt;
	int badge_w = 0;
	if (is_beta) {
		badge_txt = mfdText("Map.BetaBadge", "BETA \xE2\x80\x94 UNTESTED SPEC");
		badge_w = std::min(420, std::max(206, text_px(badge_txt, 12, true) + 24));
		badge_txt = elide_px(badge_txt, 12, true, badge_w - 12);
	}
	const int title_max_px = 560;
	/* the desk's name is the heading; the window title says what the window is */
	const QString header_title =
		elide_px(QString::fromUtf8(mfd_profile_ui_display_name(def)), 18, true, title_max_px);
	const int status_cx = std::max(388, 52 + text_px(header_title, 18, true) + 14);
	const QString dot_hex = ctrl.dot_green ? QStringLiteral("#22c55e") : QStringLiteral("#ef4444");
	const int status_right = is_beta ? (942 - badge_w - 12) : 942;
	const QString status_txt = elide_px(QString::fromUtf8(ctrl.status_sentence), 13, false,
					    std::max(40, status_right - (status_cx + 15)));

	svg.append(
		svg_text(QStringLiteral("x='38' y='48' fill='#f8fafc' %1 font-size='18' font-weight='bold'").arg(font),
			 header_title));
	svg.append(QStringLiteral("<circle cx='%1' cy='42' r='6' fill='%2'/>").arg(status_cx).arg(dot_hex));
	svg.append(
		svg_text(QStringLiteral("x='%1' y='47' fill='#cbd5e1' %2 font-size='13'").arg(status_cx + 15).arg(font),
			 status_txt));

	if (is_beta) {
		const int badge_x = 942 - badge_w;
		svg.append(QStringLiteral("<rect x='%1' y='26' width='%2' height='26' rx='13' fill='#451a03' "
					  "stroke='#f59e0b' stroke-width='1.5'/>")
				   .arg(badge_x)
				   .arg(badge_w));
		svg.append(
			svg_text(QStringLiteral("x='%1' y='43' fill='#fde68a' %2 font-size='11.5' font-weight='bold' "
						"text-anchor='middle'")
					 .arg(badge_x + badge_w / 2)
					 .arg(font),
				 badge_txt));
	}

	/* Column X centers tailored to each controller's physical fader topology */
	auto col_cx = [&](int v) -> int {
		if (def->has_crossfader) {
			/* APC40 mkII & Generic MIDI: 8 track faders + Master on right above crossfader */
			return (v < 8) ? (75 + v * 84) : 778;
		}
		if (def->has_master_fader) {
			/* APC mini mk2 & Akai MIDImix: 8 track faders + separated Master across full width */
			return (v < 8) ? (78 + v * 96) : 886;
		}
		if (def->upper_style == MFD_DESK_UPPER_TRANSPORT_AND_KNOBS_1) {
			/* Korg nanoKONTROL2: left transport section + 8 sliders on right */
			return 274 + v * 90;
		}
		/* Novation Launch Control XL: 8 evenly spaced channel strips */
		return 92 + v * 113;
	};

	/* Upper physical orientation zones */
	switch (def->upper_style) {
	case MFD_DESK_UPPER_CLIP_PADS: {
		const int grid_w = col_cx(7) + 47 - 36;
		const int scene_x = col_cx(8) - 44;
		svg.append(QStringLiteral("<rect x='36' y='60' width='%1' height='74' rx='6' fill='#0d1526' "
					  "stroke='#1e293b' stroke-width='1'/>")
				   .arg(grid_w));
		svg.append(svg_text(
			QStringLiteral("x='46' y='76' fill='#94a3b8' %1 font-size='11.5' font-weight='bold'").arg(font),
			elide_px(mfdText("Map.ClipPads", "Clip launch pads (for desk orientation \xE2\x80\x94 "
							 "Midi Fadester uses the faders below)"),
				 12, true, grid_w - 20)));
		svg.append(QStringLiteral("<rect x='%1' y='60' width='88' height='74' rx='6' fill='#0d1526' "
					  "stroke='#1e293b' stroke-width='1'/>")
				   .arg(scene_x));
		svg.append(
			svg_text(QStringLiteral("x='%1' y='76' fill='#94a3b8' %2 font-size='11.5' font-weight='bold' "
						"text-anchor='middle'")
					 .arg(col_cx(8))
					 .arg(font),
				 elide_px(mfdText("Map.ScenePads", "Scene pads"), 12, true, 84)));
		for (int col = 0; col < 8; ++col) {
			const int gx = col_cx(col) - 33;
			for (int row = 0; row < 2; ++row) {
				const int gy = 86 + row * 21;
				svg.append(QStringLiteral("<rect x='%1' y='%2' width='66' height='15' rx='3' "
							  "fill='#172033' stroke='#334155' stroke-width='1'/>")
						   .arg(gx)
						   .arg(gy));
			}
		}
		for (int row = 0; row < 2; ++row) {
			const int gy = 86 + row * 21;
			svg.append(QStringLiteral("<rect x='%1' y='%2' width='68' height='15' rx='3' "
						  "fill='#172033' stroke='#334155' stroke-width='1'/>")
					   .arg(col_cx(8) - 34)
					   .arg(gy));
		}
		break;
	}

	case MFD_DESK_UPPER_KNOB_ROWS_3: {
		const int box_w = col_cx(7) + 48 - 36;
		svg.append(QStringLiteral("<rect x='36' y='60' width='%1' height='74' rx='6' fill='#0d1526' "
					  "stroke='#1e293b' stroke-width='1'/>")
				   .arg(box_w));
		svg.append(svg_text(
			QStringLiteral("x='46' y='75' fill='#94a3b8' %1 font-size='11.5' font-weight='bold'").arg(font),
			elide_px(mfdText("Map.ChannelKnobs3", "Channel knobs \xE2\x80\x94 3 rows (for desk "
							      "orientation \xE2\x80\x94 Midi Fadester uses the "
							      "faders below)"),
				 12, true, box_w - 20)));
		for (int col = 0; col < 8; ++col) {
			const int kx = col_cx(col);
			for (int row = 0; row < 3; ++row) {
				const int ky = 88 + row * 17;
				svg.append(QStringLiteral("<circle cx='%1' cy='%2' r='5.5' fill='#172033' "
							  "stroke='#475569' stroke-width='1.2'/>"
							  "<line x1='%1' y1='%2' x2='%1' y2='%3' "
							  "stroke='#94a3b8' stroke-width='1.2'/>")
						   .arg(kx)
						   .arg(ky)
						   .arg(ky - 5));
			}
		}
		if (def->has_master_fader) {
			const int mx = col_cx(8);
			svg.append(QStringLiteral("<rect x='%1' y='60' width='76' height='74' rx='6' fill='#0d1526' "
						  "stroke='#1e293b' stroke-width='1'/>")
					   .arg(mx - 38));
			svg.append(svg_text(QStringLiteral("x='%1' y='88' fill='#94a3b8' %2 font-size='10.5' "
							   "font-weight='bold' text-anchor='middle'")
						    .arg(mx)
						    .arg(font),
					    elide_px(mfdText("Map.SendAll", "Send All"), 11, true, 72)));
			svg.append(QStringLiteral("<rect x='%1' y='100' width='52' height='18' rx='4' fill='#172033' "
						  "stroke='#334155' stroke-width='1'/>")
					   .arg(mx - 26));
		}
		break;
	}

	case MFD_DESK_UPPER_TRANSPORT_AND_KNOBS_1: {
		/* Left-hand transport & marker section matching Korg nanoKONTROL2 physical layout */
		svg.append(QStringLiteral("<rect x='36' y='60' width='182' height='334' rx='7' fill='#0d1526' "
					  "stroke='#1e293b' stroke-width='1.2'/>"));
		svg.append(svg_text(QStringLiteral("x='127' y='82' fill='#94a3b8' %1 font-size='12' font-weight='bold' "
						   "text-anchor='middle'")
					    .arg(font),
				    elide_px(mfdText("Map.Transport", "Transport & markers"), 12, true, 176)));
		svg.append(svg_text(
			QStringLiteral("x='127' y='98' fill='#64748b' %1 font-size='10.5' text-anchor='middle'")
				.arg(font),
			elide_px(mfdText("Map.ForOrientation", "(for desk orientation)"), 11, false, 176)));
		svg.append(QStringLiteral(
			"<rect x='52' y='124' width='68' height='22' rx='4' fill='#172033' stroke='#334155'/>"));
		svg.append(
			svg_text(QStringLiteral("x='86' y='139' fill='#64748b' %1 font-size='9.5' text-anchor='middle'")
					 .arg(font),
				 elide_px(mfdText("Map.TrackButtons", "TRACK < >"), 10, false, 66)));
		svg.append(QStringLiteral(
			"<rect x='132' y='124' width='70' height='22' rx='4' fill='#172033' stroke='#334155'/>"));
		svg.append(svg_text(
			QStringLiteral("x='167' y='139' fill='#64748b' %1 font-size='9.5' text-anchor='middle'")
				.arg(font),
			elide_px(mfdText("Map.Cycle", "CYCLE"), 10, false, 68)));
		svg.append(QStringLiteral(
			"<rect x='52' y='162' width='150' height='22' rx='4' fill='#172033' stroke='#334155'/>"));
		svg.append(svg_text(
			QStringLiteral("x='127' y='177' fill='#64748b' %1 font-size='9.5' text-anchor='middle'")
				.arg(font),
			elide_px(mfdText("Map.MarkerButtons", "MARKER SET / < / >"), 10, false, 146)));
		/* the rewind and fast-forward glyphs are printed on the hardware and need no words */
		const QString k_trans[5] = {QStringLiteral("<<"), QStringLiteral(">>"), mfdText("Map.Stop", "STOP"),
					    mfdText("Map.Play", "PLAY"), mfdText("Map.Rec", "REC")};
		for (int tb = 0; tb < 5; ++tb) {
			const int tx = 48 + tb * 32;
			svg.append(QStringLiteral("<rect x='%1' y='224' width='28' height='28' rx='4' "
						  "fill='#172033' stroke='#475569' stroke-width='1'/>")
					   .arg(tx));
			svg.append(svg_text(QStringLiteral("x='%1' y='242' fill='#94a3b8' %2 font-size='8.5' "
							   "font-weight='bold' text-anchor='middle'")
						    .arg(tx + 14)
						    .arg(font),
					    elide_px(k_trans[tb], 9, true, 30)));
		}
		svg.append(QStringLiteral("<rect x='230' y='60' width='712' height='74' rx='6' fill='#0d1526' "
					  "stroke='#1e293b' stroke-width='1'/>"));
		svg.append(svg_text(
			QStringLiteral("x='242' y='76' fill='#94a3b8' %1 font-size='11.5' font-weight='bold'").arg(font),
			elide_px(mfdText("Map.PanKnobs", "Channel pan knobs (for desk orientation \xE2\x80\x94 "
							 "Midi Fadester uses the sliders below)"),
				 12, true, 692)));
		for (int col = 0; col < 8; ++col) {
			const int kx = col_cx(col);
			svg.append(QStringLiteral("<circle cx='%1' cy='107' r='12' fill='#172033' "
						  "stroke='#475569' stroke-width='1.5'/>"
						  "<line x1='%1' y1='107' x2='%1' y2='96' "
						  "stroke='#94a3b8' stroke-width='1.8'/>")
					   .arg(kx));
		}
		break;
	}

	case MFD_DESK_UPPER_NONE:
	default:
		svg.append(QStringLiteral("<rect x='36' y='60' width='906' height='74' rx='6' fill='#0d1526' "
					  "stroke='#1e293b' stroke-width='1'/>"));
		svg.append(svg_text(
			QStringLiteral("x='489' y='94' fill='#cbd5e1' %1 font-size='12.5' font-weight='bold' "
				       "text-anchor='middle'")
				.arg(font),
			elide_px(mfdText("Map.CustomLayout", "Custom MIDI Controller Layout"), 13, true, 880)));
		svg.append(svg_text(
			QStringLiteral("x='489' y='115' fill='#94a3b8' %1 font-size='11.5' text-anchor='middle'")
				.arg(font),
			elide_px(mfdText("Map.CustomHint",
					 "Open Set up (Step 3 \xE2\x86\x92 Which hardware control?) or "
					 "Advanced \xE2\x86\x92 Learn to bind any physical "
					 "fader or knob."),
				 12, false, 880)));
		break;
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
		if (sp.at_end) {
			return QStringLiteral("#10b981");
		}
		return QStringLiteral("#38bdf8");
	};

	auto build_strip_pres = [&](int idx, struct mfd_ui_strip_presentation *out) {
		const struct mfd_fader_slot &s = snap.slots[idx];
		char disp_buf[64] = {0};
		mfd_engine_format_slot_display_value_ex(&s, s.last_applied_val, true, disp_buf, sizeof(disp_buf));
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
		in.filter_name = s.filter_name;
		in.filter_prop = s.filter_prop;
		in.filter_prop_label = s.filter_prop_label;
		in.display_value = disp_buf;
		in.available = s.cached_available;
		in.reason = s.cached_reason;
		in.reason_msg = &s.cached_reason_msg;
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

	/* Vertical faders: Track 1..track_fader_count + optional Master (index 9) */
	const int vert_count = def->track_fader_count + (def->has_master_fader ? 1 : 0);
	for (int v = 0; v < vert_count; ++v) {
		const int slot_idx = (v < def->track_fader_count) ? (v + 1) : 9;
		const int cx = col_cx(v);
		struct mfd_ui_strip_presentation sp;
		build_strip_pres(slot_idx, &sp);
		const QString accent = accent_for_strip(sp);
		const QString title = elide_px(QString::fromUtf8(sp.title), 12, true, 70);
		const QString cc_lbl = elide_px(compact_midi_addr(snap.slots[slot_idx].addr), 10, false, 72);
		const QString short_lbl = elide_px(QString::fromUtf8(sp.svg_short_label), 10, true, 72);
		char pct_buf[32] = {0};
		mfd_ui_format_percent(sp.bar_percent, pct_buf, sizeof(pct_buf));

		const int track_top = 188;
		const int track_h = 144;
		const int fill_h = (sp.bar_percent * track_h) / 100;
		const int cap_y = track_top + track_h - fill_h - 8;

		svg.append(QStringLiteral("<rect x='%1' y='146' width='74' height='248' rx='7' fill='#0f172a' "
					  "stroke='%2' stroke-width='1.5'/>")
				   .arg(cx - 37)
				   .arg(accent));
		svg.append(svg_text(QStringLiteral("x='%1' y='162' fill='#f8fafc' %2 font-size='12' font-weight='bold' "
						   "text-anchor='middle'")
					    .arg(cx)
					    .arg(font),
				    title));
		svg.append(
			svg_text(QStringLiteral("x='%1' y='175' fill='#94a3b8' %2 font-size='9.5' text-anchor='middle'")
					 .arg(cx)
					 .arg(font),
				 cc_lbl));
		svg.append(QStringLiteral("<rect x='%1' y='%2' width='8' height='%3' rx='4' fill='#1e293b'/>"
					  "<rect x='%1' y='%4' width='8' height='%5' rx='4' fill='%6'/>"
					  "<rect x='%7' y='%8' width='26' height='16' rx='3' fill='#e2e8f0' "
					  "stroke='%6' stroke-width='1.5'/>")
				   .arg(cx - 4)
				   .arg(track_top)
				   .arg(track_h)
				   .arg(track_top + track_h - fill_h)
				   .arg(fill_h)
				   .arg(accent)
				   .arg(cx - 13)
				   .arg(cap_y));
		svg.append(
			svg_text(QStringLiteral("x='%1' y='356' fill='#cbd5e1' %2 font-size='10' text-anchor='middle'")
					 .arg(cx)
					 .arg(font),
				 QString::fromUtf8(pct_buf)));
		svg.append(svg_text(QStringLiteral("x='%1' y='378' fill='%2' %3 font-size='10' font-weight='bold' "
						   "text-anchor='middle'")
					    .arg(cx)
					    .arg(accent)
					    .arg(font),
				    short_lbl));
	}

	if (def->has_crossfader) {
		/* Horizontal Crossfader (index 0) at bottom right */
		struct mfd_ui_strip_presentation sp0;
		build_strip_pres(0, &sp0);
		const QString accent0 = accent_for_strip(sp0);
		const QString title0 = mfdFill(mfdText("Map.CrossfaderTitle", "%1 (%2%)"),
					       {elide_px(QString::fromUtf8(sp0.title), 12, true, 120),
						QString::number(sp0.bar_percent)});
		const int title0_px = text_px(title0, 12, true);
		const QString assign0 = elide_px(QString::fromUtf8(sp0.assignment_sentence), 11, true,
						 std::max(60, 266 - title0_px - 12));
		const int xf_left = 680;
		const int xf_w = 190;
		const int fill_w = (sp0.bar_percent * xf_w) / 100;
		const int cap_x = xf_left + fill_w - 8;

		svg.append(QStringLiteral("<rect x='642' y='406' width='298' height='68' rx='8' fill='#0f172a' "
					  "stroke='%1' stroke-width='1.8'/>")
				   .arg(accent0));
		svg.append(svg_text(
			QStringLiteral("x='658' y='426' fill='#f8fafc' %1 font-size='12' font-weight='bold'").arg(font),
			title0));
		svg.append(svg_text(QStringLiteral("x='924' y='426' fill='%1' %2 font-size='11' font-weight='bold' "
						   "text-anchor='end'")
					    .arg(accent0)
					    .arg(font),
				    assign0));
		svg.append(QStringLiteral("<rect x='%1' y='442' width='%2' height='8' rx='4' fill='#1e293b'/>"
					  "<rect x='%1' y='442' width='%3' height='8' rx='4' fill='%4'/>"
					  "<rect x='%5' y='434' width='16' height='24' rx='3' fill='#f8fafc' "
					  "stroke='%4' stroke-width='1.5'/>")
				   .arg(xf_left)
				   .arg(xf_w)
				   .arg(fill_w)
				   .arg(accent0)
				   .arg(cap_x));
	} else {
		/* Beta spec & crossfader-less T-bar guidance card at bottom right */
		svg.append(QStringLiteral("<rect x='592' y='406' width='350' height='68' rx='8' fill='#1c1507' "
					  "stroke='#d97706' stroke-width='1.5'/>"));
		svg.append(
			svg_text(QStringLiteral("x='606' y='426' fill='#fde68a' %1 font-size='11.5' font-weight='bold'")
					 .arg(font),
				 elide_px(mfdText("Map.BetaCardTitle",
						  "Beta profile \xE2\x80\x94 mapped from manufacturer MIDI spec"),
					  12, true, 326)));
		svg.append(svg_text(QStringLiteral("x='606' y='445' fill='#e2e8f0' %1 font-size='10.5'").arg(font),
				    elide_px(mfdText("Map.BetaCardNoCrossfader",
						     "No physical crossfader; set any fader to Switch scenes (T-bar)."),
					     11, false, 330)));
		svg.append(svg_text(QStringLiteral("x='606' y='462' fill='#94a3b8' %1 font-size='10'").arg(font),
				    elide_px(mfdText("Map.BetaCardLearn",
						     "Use Learn in Set up or Advanced if your unit uses custom CCs."),
					     10, false, 330)));
	}

	/* Legend at bottom left: each entry starts after the measured previous one */
	const QString legend_colors[4] = {QStringLiteral("#38bdf8"), QStringLiteral("#10b981"),
					  QStringLiteral("#f59e0b"), QStringLiteral("#ef4444")};
	const QString legend_texts[4] = {mfdText("Map.LegendActive", "Active / Ready"),
					 mfdText("Map.LegendAtEnd", "At end (parked)"),
					 mfdText("Map.LegendPickup", "Needs pick-up or Studio Mode"),
					 mfdText("Map.LegendAttention", "Needs attention")};
	const int legend_english_cx[4] = {52, 168, 294, 484};
	/* the legend ends where the crossfader or Beta card begins */
	const int legend_right = def->has_crossfader ? 632 : 582;
	int legend_cx = legend_english_cx[0];
	for (int li = 0; li < 4; ++li) {
		legend_cx = std::max(legend_cx, legend_english_cx[li]);
		const int room = std::max(30, legend_right - (legend_cx + 12));
		const QString text = elide_px(legend_texts[li], 11, false, room);
		svg.append(QStringLiteral("<circle cx='%1' cy='442' r='5' fill='%2'/>")
				   .arg(legend_cx)
				   .arg(legend_colors[li]));
		svg.append(svg_text(
			QStringLiteral("x='%1' y='446' fill='#cbd5e1' %2 font-size='11'").arg(legend_cx + 12).arg(font),
			text));
		legend_cx += 12 + text_px(text, 11, false) + 24;
	}
	svg.append(QStringLiteral("</svg>"));

	return svg.toUtf8();
}

QByteArray mfd_make_apc40_desk_svg(const struct mfd_ui_snapshot &snap)
{
	return mfd_make_controller_desk_svg(snap);
}

MfdControllerViewDialog::MfdControllerViewDialog(QWidget *parent) : QDialog(parent)
{
	setObjectName(QStringLiteral("mfdControllerSvgDialog"));
	setWindowTitle(mfdText("Map.WindowTitle", "Midi Fadester \xE2\x80\x94 Controller"));
	resize(960, 560);
	setStyleSheet(QStringLiteral("QDialog { background-color: #0b1120; color: #f8fafc; }"));

	auto *root = new QVBoxLayout(this);
	root->setContentsMargins(14, 14, 14, 14);
	root->setSpacing(10);

	subtitle_label_ =
		new QLabel(mfdText("Map.Subtitle",
				   "Move any physical fader to verify its live position and assignment on the desk."),
			   this);
	subtitle_label_->setObjectName(QStringLiteral("mfdControllerSvgSubtitle"));
	subtitle_label_->setWordWrap(true);
	subtitle_label_->setStyleSheet(QStringLiteral("color: #94a3b8; font-size: 13px;"));
	root->addWidget(subtitle_label_);

	svg_canvas_ = new MfdSvgCanvas(this);
	svg_canvas_->setObjectName(QStringLiteral("mfdControllerSvgCanvas"));
	svg_canvas_->setMinimumSize(860, 440);
	root->addWidget(svg_canvas_, 1);

	auto *footer = new QHBoxLayout();
	footer->addStretch(1);
	auto *btn_close = new QPushButton(mfdText("Map.Close", "Close"), this);
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
	const struct mfd_controller_profile_def *def = mfd_profile_get_def(snap.active_profile);
	if (subtitle_label_ && def) {
		if (def->tier == MFD_PROFILE_TIER_BETA_UNTESTED) {
			subtitle_label_->setText(QString::fromUtf8(mfd_profile_ui_spec_note(def)));
			subtitle_label_->setStyleSheet(
				QStringLiteral("color: #fde68a; font-size: 13px; font-weight: bold;"));
		} else {
			subtitle_label_->setText(mfdText(
				"Map.Subtitle",
				"Move any physical fader to verify its live position and assignment on the desk."));
			subtitle_label_->setStyleSheet(QStringLiteral("color: #94a3b8; font-size: 13px;"));
		}
	}
	if (svg_canvas_) {
		svg_canvas_->set_svg_utf8(mfd_make_controller_desk_svg(snap));
	}
}
