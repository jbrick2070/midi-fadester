/*
 * Midi Fadester
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "ui/mfd-text-qt.hpp"

#include "core/mfd-text.h"

#include <plugin-support.h>

#include <QMessageBox>
#include <QPushButton>

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

QString mfdWindowTitle(const QString &window)
{
	return mfdFill(mfdText("Window.Title", "Midi Fadester %1 \xE2\x80\x94 %2"),
		       {QString::fromUtf8(PLUGIN_VERSION), window});
}

bool mfdConfirm(QWidget *parent, const QString &window, const QString &question, const QString &do_it)
{
	QMessageBox box(parent);
	box.setObjectName(QStringLiteral("mfdConfirmBox"));
	/* Qt's own box on every system: macOS would otherwise show a native alert, where Cancel may not be the default
	 * button and the same box looks and acts differently from Windows */
	box.setOption(QMessageBox::Option::DontUseNativeDialog, true);
	/* off screen when its window is (the UI test) */
	box.setAttribute(Qt::WA_DontShowOnScreen, parent && parent->testAttribute(Qt::WA_DontShowOnScreen));
	box.setIcon(QMessageBox::Warning);
	box.setWindowTitle(mfdWindowTitle(window)); /* ignored by Qt on macOS, where alerts have no title */
	box.setProperty("mfdTitle", mfdWindowTitle(window));
	box.setText(question);
	QPushButton *yes = box.addButton(do_it, QMessageBox::AcceptRole);
	yes->setObjectName(QStringLiteral("mfdConfirmYes"));
	QPushButton *no = box.addButton(mfdText("Setup.Cancel", "Cancel"), QMessageBox::RejectRole);
	no->setObjectName(QStringLiteral("mfdConfirmCancel"));
	box.setDefaultButton(no);
	box.setEscapeButton(no);
	box.exec();
	return box.clickedButton() == yes;
}
