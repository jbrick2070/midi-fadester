/*
 * Midi Fadester
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <QByteArray>
#include <QPainter>
#include <QPaintEvent>
#include <QRectF>
#include <QSize>
#include <QSvgRenderer>
#include <QWidget>

class MfdSvgCanvas final : public QWidget {
public:
	explicit MfdSvgCanvas(QWidget *parent = nullptr) : QWidget(parent) {}

	void set_svg_utf8(const QByteArray &svg_bytes)
	{
		if (svg_bytes == last_svg_) {
			return;
		}
		last_svg_ = svg_bytes;
		renderer_.load(last_svg_);
		update();
	}

	bool is_valid() const { return renderer_.isValid(); }

protected:
	void paintEvent(QPaintEvent *event) override
	{
		Q_UNUSED(event);
		if (!renderer_.isValid()) {
			return;
		}
		QPainter painter(this);
		painter.setRenderHint(QPainter::Antialiasing, true);
		painter.setRenderHint(QPainter::SmoothPixmapTransform, true);

		QSize def_sz = renderer_.defaultSize();
		if (def_sz.isEmpty()) {
			renderer_.render(&painter, QRectF(rect()));
			return;
		}
		QSizeF target = QSizeF(def_sz);
		target.scale(QSizeF(size()), Qt::KeepAspectRatio);
		const qreal x = (static_cast<qreal>(width()) - target.width()) * 0.5;
		const qreal y = (static_cast<qreal>(height()) - target.height()) * 0.5;
		renderer_.render(&painter, QRectF(x, y, target.width(), target.height()));
	}

private:
	QByteArray last_svg_;
	QSvgRenderer renderer_;
};
