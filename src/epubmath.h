// SPDX-FileCopyrightText: 2026 Arianna contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QByteArray>
#include <QString>

namespace EpubMath
{
// The legacy config is a marker, never reader configuration. Detection does not
// evaluate book scripts. Only the temporary BookServer delivery copy is changed.
bool needsMathJax(const QByteArray &xhtml);
QByteArray injectMathJax(const QByteArray &xhtml, const QString &bootstrapUrl);
}
