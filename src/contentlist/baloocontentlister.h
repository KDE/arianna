// SPDX-FileCopyrightText: 2015 Dan Leinir Turthra Jensen <admin@leinir.dk>
// SPDX-FileCopyrightText: 2022 Carl Schwan <carl@carlschwan.eu>
// SPDX-License-Identifier: LGPL-2.1-only or LGPL-3.0-only or LicenseRef-KDE-Accepted-LGPL

#pragma once

#include "contentlisterbase.h"
#include <memory>

#include <Baloo/QueryRunnable>

#include <QString>

class BalooContentLister : public ContentListerBase
{
    Q_OBJECT
public:
    explicit BalooContentLister(QObject *parent = nullptr);
    ~BalooContentLister() override;

    /**
     * @returns whether Baloo is enabled. If baloo is not available on the system, we cannot use it.
     */
    bool balooEnabled() const;

    /**
     * \brief Start a search.
     * @param queries  List of ContentQueries that the search should be limited to.
     */
    void startSearch(const QList<ContentQuery *> &queries) override;

private:
    class Private;
    std::unique_ptr<Private> d;

    Q_SLOT void queryCompleted(Baloo::QueryRunnable *query);
    void queryResult(const ContentQuery *query, const QString &location, const QString &file);
};
