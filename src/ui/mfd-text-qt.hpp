/*
 * Midi Fadester
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

/* The Qt side of the text hook (src/core/mfd-text.h): a locale text as a QString, and the same single-pass %1..%9
 * fill as mfd_text_fill, so a name holding "%2" is never filled again and nothing is ever a printf format
 * (R-I18N-3). Qt thread only. */

#include <QString>
#include <QStringList>

/* The text for key in the language OBS runs in, else english (both UTF-8). */
QString mfdText(const char *key, const char *english);

/* text with "%1".."%9" replaced by values[0..8] in one pass; a placeholder with no value is left out, any other "%"
 * stays. */
QString mfdFill(const QString &text, const QStringList &values);

/* The title of every Midi Fadester window: "Midi Fadester <version>, a dash, <window>" (the suite's rule). */
QString mfdWindowTitle(const QString &window);

class QWidget;
/* Asks before work is cleared: the question, the button that does it (named for what it does) and Cancel, which is the
 * default and Escape. True when the user chose to go on. The box is named "mfdConfirmBox"; its title shows the version. */
bool mfdConfirm(QWidget *parent, const QString &window, const QString &question, const QString &do_it);
