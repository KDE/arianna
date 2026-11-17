// SPDX-FileCopyrightText: 2026 Markus
// SPDX-License-Identifier: LGPL-2.1-only or LGPL-3.0-only or LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <QObject>
#include <QVariantMap>
#include <qqmlintegration.h>

/**
 * Read-only inspection of a PDF encryption dictionary.
 *
 * The returned map deliberately reports the PDF permission flags, rather
 * than the current operating-system file permissions. No document data is
 * changed while inspecting it.
 */
class PdfSecurityInfo : public QObject
{
    Q_OBJECT
    QML_ELEMENT

public:
    explicit PdfSecurityInfo(QObject *parent = nullptr);

    Q_INVOKABLE QVariantMap inspect(const QString &fileName, const QString &password = QString()) const;
};
