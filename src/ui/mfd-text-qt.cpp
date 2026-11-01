/*
 * Midi Fadester
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "ui/mfd-text-qt.hpp"

#include "core/mfd-text.h"

QString mfdText(const char *key, const char *english)
{
	return QString::fromUtf8(mfd_text(key, english));
}

QString mfdFill(const QString &text, const QStringList &values)
{
	QString out;
	out.reserve(text.size() + 16);
	const qsizetype n = text.size();
	for (qsizetype i = 0; i < n; ++i) {
		const QChar c = text.at(i);
		if (c == QLatin1Char('%') && i + 1 < n && text.at(i + 1) >= QLatin1Char('1') &&
		    text.at(i + 1) <= QLatin1Char('9')) {
			const qsizetype index = text.at(i + 1).unicode() - QLatin1Char('1').unicode();
			if (index < values.size()) {
				out.append(values.at(index));
			}
			++i;
			continue;
		}
		out.append(c);
	}
	return out;
}
