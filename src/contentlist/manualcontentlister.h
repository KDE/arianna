// SPDX-FileCopyrightText: 2022 Carl Schwan <carl@carlschwan.eu>
// SPDX-License-Identifier: LGPL-2.1-only or LGPL-3.0-only or LicenseRef-KDE-Accepted-LGPL

#pragma once

#include "contentlisterbase.h"
#include <memory>

class ManualContentLister : public ContentListerBase
{
    Q_OBJECT
public:
    explicit ManualContentLister(QObject *parent = nullptr);
    ~ManualContentLister() override;

    void startSearch(const QList<ContentQuery *> &queries) override;

    bool addFiles(const QList<QUrl> &filePaths);

private:
    class Private;
    std::unique_ptr<Private> d;
};
