// SPDX-FileCopyrightText: 2015 Dan Leinir Turthra Jensen <admin@leinir.dk>
// SPDX-FileCopyrightText: 2022 Carl Schwan <carl@carlschwan.eu>
// SPDX-License-Identifier: LGPL-2.1-only or LGPL-3.0-only or LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <QRunnable>

#include "contentlisterbase.h"
#include <memory>

class FilesystemContentLister : public ContentListerBase
{
    Q_OBJECT
public:
    explicit FilesystemContentLister(QObject *parent = nullptr);
    ~FilesystemContentLister() override;
    /**
     * \brief Start a search.
     * @param queries  List of ContentQueries that the search should be limited to.
     */
    void startSearch(const QList<ContentQuery *> &queries) override;

private:
    void queryFinished(QRunnable *runnable);

    class Private;
    std::unique_ptr<Private> d;
};
