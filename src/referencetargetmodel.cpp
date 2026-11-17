// SPDX-License-Identifier: GPL-3.0-or-later
#include "referencetargetmodel.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>

struct ReferenceTargetModel::Node {
    Node *parent = nullptr;
    QString key;
    QString label;
    QVariantMap target;
    QString searchKey;
    std::vector<std::unique_ptr<Node>> children;
    std::vector<Node *> visible;
    int visibleRow = -1;
};

ReferenceTargetModel::ReferenceTargetModel(QObject *parent)
    : QAbstractItemModel(parent), m_root(std::make_unique<Node>())
{
}

ReferenceTargetModel::~ReferenceTargetModel() = default;

QHash<int, QByteArray> ReferenceTargetModel::roleNames() const
{
    return {{Qt::DisplayRole, "display"}, {LabelRole, "label"}, {TargetValueRole, "targetValue"},
            {TargetDataRole, "targetData"}, {TooltipRole, "tooltip"}, {NodeKeyRole, "nodeKey"}};
}

QModelIndex ReferenceTargetModel::index(int row, int column, const QModelIndex &parent) const
{
    if (column != 0 || row < 0 || (parent.isValid() && (parent.model() != this || parent.column() != 0))) return {};
    auto *node = parent.isValid() ? static_cast<Node *>(parent.internalPointer()) : m_root.get();
    if (size_t(row) >= node->visible.size()) return {};
    return createIndex(row, column, node->visible[row]);
}

QModelIndex ReferenceTargetModel::indexForNode(Node *node) const
{
    if (!node || node == m_root.get() || node->visibleRow < 0) return {};
    for (auto *ancestor = node->parent; ancestor != m_root.get(); ancestor = ancestor->parent) {
        if (ancestor->visibleRow < 0) return {};
    }
    return createIndex(node->visibleRow, 0, node);
}

QModelIndex ReferenceTargetModel::parent(const QModelIndex &index) const
{
    if (!index.isValid() || index.model() != this) return {};
    return indexForNode(static_cast<Node *>(index.internalPointer())->parent);
}

int ReferenceTargetModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid() && (parent.model() != this || parent.column() != 0)) return 0;
    const auto *node = parent.isValid() ? static_cast<Node *>(parent.internalPointer()) : m_root.get();
    return int(node->visible.size());
}

int ReferenceTargetModel::columnCount(const QModelIndex &) const { return 1; }

QVariant ReferenceTargetModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.model() != this) return {};
    const auto *node = static_cast<Node *>(index.internalPointer());
    switch (role) {
    case Qt::DisplayRole:
    case LabelRole: return node->label;
    case TargetValueRole: return node->target.value(QStringLiteral("value")).toString();
    case TargetDataRole: return node->target;
    case TooltipRole: return node->target.value(QStringLiteral("tooltip"), node->label);
    case NodeKeyRole: return node->key;
    default: return {};
    }
}

Qt::ItemFlags ReferenceTargetModel::flags(const QModelIndex &index) const
{
    if (!index.isValid()) return Qt::NoItemFlags;
    return Qt::ItemIsEnabled | (data(index, TargetValueRole).toString().isEmpty() ? Qt::NoItemFlags : Qt::ItemIsSelectable);
}

ReferenceTargetModel::Node *ReferenceTargetModel::appendNode(Node *parent, const QString &key, const QString &label)
{
    auto node = std::make_unique<Node>();
    node->parent = parent;
    node->key = key;
    node->label = label;
    auto *result = node.get();
    parent->children.push_back(std::move(node));
    return result;
}

void ReferenceTargetModel::setRows(const QVariantList &rows)
{
    if (m_rows == rows) return;
    beginResetModel();
    m_rows = rows;
    m_root = std::make_unique<Node>();
    m_byValue.clear();
    QHash<QString, Node *> branches;
    for (const QVariant &entry : rows) {
        const QVariantMap row = entry.toMap();
        const QString value = row.value(QStringLiteral("value")).toString();
        if (value.isEmpty()) continue;
        const QVariantMap target = row.value(QStringLiteral("data")).toMap();
        const QVariantList path = target.value(QStringLiteral("tocPath")).toList();
        const QVariantList ids = target.value(QStringLiteral("tocIdPath")).toList();
        Node *node = m_root.get();
        QVariantList keyParts;
        // Old servers have labels only. Scope these to the file, never another XHTML.
        if (ids.size() != path.size() || ids.isEmpty())
            keyParts.append(target.value(QStringLiteral("file"), value.section(u'#', 0, 0)));
        for (int i = 0; i < path.size(); ++i) {
            keyParts.append(ids.size() == path.size() ? ids.at(i) : path.at(i));
            const QString key = QString::fromUtf8(QJsonDocument(QJsonArray::fromVariantList(keyParts)).toJson(QJsonDocument::Compact));
            if (!branches.contains(key)) branches.insert(key, appendNode(node, key, path.at(i).toString()));
            node = branches.value(key);
        }
        const QString type = target.value(QStringLiteral("type")).toString();
        const bool navigation = type == QLatin1String("navigation") && !path.isEmpty();
        if (path.isEmpty() && !value.startsWith(u'{')) {
            const QString document = value.section(u'#', 0, 0);
            const QString key = QStringLiteral("file:") + document;
            if (!branches.contains(key)) branches.insert(key, appendNode(m_root.get(), key, document));
            node = branches.value(key);
        }
        // A navigation target occupies its chapter node; exact element targets are leaves.
        // If legacy paths collide, keep the extra target as a leaf instead of losing it.
        if ((!navigation && type != QLatin1String("document")) || !node->target.isEmpty() || node == m_root.get())
            node = appendNode(node, QStringLiteral("target:") + value, row.value(QStringLiteral("label")).toString().trimmed());
        node->target = row;
        node->searchKey = row.value(QStringLiteral("searchKey")).toString();
        if (!m_byValue.contains(value)) m_byValue.insert(value, node);
    }
    filter();
    endResetModel();
    Q_EMIT rowsChanged();
    Q_EMIT matchCountChanged();
}

void ReferenceTargetModel::setFilterText(const QString &text)
{
    if (m_filterText == text) return;
    beginResetModel();
    m_filterText = text;
    filter();
    endResetModel();
    Q_EMIT filterTextChanged();
    Q_EMIT matchCountChanged();
}

void ReferenceTargetModel::filter()
{
    const auto terms = m_filterText.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
    m_matchCount = 0;
    const auto visit = [&](auto &&self, Node *node) -> bool {
        bool matches = !node->target.isEmpty();
        for (const QString &term : terms) matches = matches && node->searchKey.contains(term);
        if (matches) ++m_matchCount;
        node->visible.clear();
        node->visibleRow = -1;
        for (const auto &child : node->children) {
            if (self(self, child.get())) {
                child->visibleRow = int(node->visible.size());
                node->visible.push_back(child.get());
            }
        }
        return terms.isEmpty() || matches || !node->visible.empty();
    };
    visit(visit, m_root.get());
}

QModelIndex ReferenceTargetModel::indexForValue(const QString &value) const { return indexForNode(m_byValue.value(value)); }
QVariantMap ReferenceTargetModel::rowForValue(const QString &value) const
{
    const auto *node = m_byValue.value(value);
    return node ? node->target : QVariantMap{};
}
QString ReferenceTargetModel::keyForIndex(const QModelIndex &index) const { return data(index, NodeKeyRole).toString(); }

#include "moc_referencetargetmodel.cpp"
