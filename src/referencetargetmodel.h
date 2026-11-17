// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QAbstractItemModel>
#include <QHash>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>
#include <memory>
#include <vector>

// Ephemeral display model. Target values and payloads always remain unmodified.
class ReferenceTargetModel : public QAbstractItemModel
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QVariantList rows READ rows WRITE setRows NOTIFY rowsChanged)
    Q_PROPERTY(QString filterText READ filterText WRITE setFilterText NOTIFY filterTextChanged)
    Q_PROPERTY(int matchCount READ matchCount NOTIFY matchCountChanged)

public:
    explicit ReferenceTargetModel(QObject *parent = nullptr);
    ~ReferenceTargetModel() override;
    enum Role { LabelRole = Qt::UserRole + 1, TargetValueRole, TargetDataRole, TooltipRole, NodeKeyRole };
    Q_ENUM(Role)
    QHash<int, QByteArray> roleNames() const override;
    QModelIndex index(int row, int column, const QModelIndex &parent = {}) const override;
    QModelIndex parent(const QModelIndex &index) const override;
    int rowCount(const QModelIndex &parent = {}) const override;
    int columnCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    Qt::ItemFlags flags(const QModelIndex &index) const override;

    QVariantList rows() const { return m_rows; }
    void setRows(const QVariantList &rows);
    QString filterText() const { return m_filterText; }
    void setFilterText(const QString &text);
    int matchCount() const { return m_matchCount; }
    Q_INVOKABLE QModelIndex indexForValue(const QString &value) const;
    Q_INVOKABLE QVariantMap rowForValue(const QString &value) const;
    Q_INVOKABLE QString keyForIndex(const QModelIndex &index) const;

Q_SIGNALS:
    void rowsChanged();
    void filterTextChanged();
    void matchCountChanged();

private:
    struct Node;
    Node *appendNode(Node *parent, const QString &key, const QString &label);
    QModelIndex indexForNode(Node *node) const;
    void filter();
    QVariantList m_rows;
    QString m_filterText;
    int m_matchCount = 0;
    std::unique_ptr<Node> m_root;
    QHash<QString, Node *> m_byValue;
};
