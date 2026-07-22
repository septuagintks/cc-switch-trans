#include "prototype/profile_list_model.hpp"

#include <QSet>
#include <QVariant>

#include <algorithm>
#include <limits>

namespace ccs_trans::gui {

ProfileListModel::ProfileListModel(QObject* parent) : QAbstractListModel(parent) {}

int ProfileListModel::rowCount(const QModelIndex& parent) const {
    if (parent.isValid()) {
        return 0;
    }
    return static_cast<int>(items_.size());
}

QVariant ProfileListModel::data(const QModelIndex& index, const int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= rowCount()) {
        return {};
    }
    const auto& item = items_[static_cast<std::size_t>(index.row())];
    switch (role) {
        case StableKeyRole:
            return item.stableKey;
        case NameRole:
            return item.name;
        case EnabledRole:
            return item.enabled;
        case RuleCountRole:
            return item.ruleCount;
        case RevisionRole:
            return QVariant::fromValue<qulonglong>(item.revision);
        default:
            return {};
    }
}

QHash<int, QByteArray> ProfileListModel::roleNames() const {
    return {
        {StableKeyRole, "stableKey"},
        {NameRole, "profileName"},
        {EnabledRole, "profileEnabled"},
        {RuleCountRole, "ruleCount"},
        {RevisionRole, "revision"},
    };
}

int ProfileListModel::count() const noexcept {
    return static_cast<int>(items_.size());
}

void ProfileListModel::populate(const int profileCount, const int rulesPerProfile) {
    const int boundedProfiles = std::clamp(profileCount, 0, 10'000);
    const int boundedRules = std::clamp(rulesPerProfile, 0, 1'000'000);

    beginResetModel();
    items_.clear();
    items_.reserve(static_cast<std::size_t>(boundedProfiles));
    nextStableKey_ = 1;
    for (int index = 0; index < boundedProfiles; ++index) {
        const auto key = nextStableKey_++;
        items_.push_back(Item{
            QStringLiteral("profile-%1").arg(key),
            QStringLiteral("Profile %1").arg(index + 1),
            index % 3 != 0,
            boundedRules,
            1,
        });
    }
    endResetModel();
    emit countChanged();
}

QString ProfileListModel::stableKeyAt(const int row) const {
    if (row < 0 || row >= rowCount()) {
        return {};
    }
    return items_[static_cast<std::size_t>(row)].stableKey;
}

int ProfileListModel::indexOfStableKey(const QString& stableKey) const {
    const auto found = std::find_if(items_.cbegin(), items_.cend(), [&](const Item& item) {
        return item.stableKey == stableKey;
    });
    if (found == items_.cend()) {
        return -1;
    }
    return static_cast<int>(std::distance(items_.cbegin(), found));
}

bool ProfileListModel::containsStableKey(const QString& stableKey) const {
    return indexOfStableKey(stableKey) >= 0;
}

int ProfileListModel::totalRuleCount() const noexcept {
    std::uint64_t total = 0;
    for (const auto& item : items_) {
        total += static_cast<std::uint64_t>(item.ruleCount);
    }
    return total > static_cast<std::uint64_t>(std::numeric_limits<int>::max())
               ? std::numeric_limits<int>::max()
               : static_cast<int>(total);
}

int ProfileListModel::mutableRow(const std::uint64_t sequence,
                                 const QString& protectedStableKey) const {
    if (items_.empty()) {
        return -1;
    }
    int row = static_cast<int>(sequence % items_.size());
    if (items_[static_cast<std::size_t>(row)].stableKey != protectedStableKey) {
        return row;
    }
    row = (row + 1) % rowCount();
    return items_[static_cast<std::size_t>(row)].stableKey == protectedStableKey ? -1 : row;
}

void ProfileListModel::updateRow(const int row) {
    auto& item = items_[static_cast<std::size_t>(row)];
    ++item.revision;
    item.enabled = !item.enabled;
    item.name = QStringLiteral("Profile %1 r%2").arg(row + 1).arg(item.revision);
    const QModelIndex changed = index(row, 0);
    emit dataChanged(changed, changed, {NameRole, EnabledRole, RevisionRole});
}

void ProfileListModel::moveRow(const int sourceRow, const int targetRow) {
    if (sourceRow == targetRow || sourceRow < 0 || targetRow < 0 ||
        sourceRow >= rowCount() || targetRow >= rowCount()) {
        return;
    }
    const int destinationChild = sourceRow < targetRow ? targetRow + 1 : targetRow;
    if (!beginMoveRows({}, sourceRow, sourceRow, {}, destinationChild)) {
        return;
    }
    Item moved = std::move(items_[static_cast<std::size_t>(sourceRow)]);
    items_.erase(items_.begin() + sourceRow);
    items_.insert(items_.begin() + targetRow, std::move(moved));
    endMoveRows();
}

void ProfileListModel::replaceRow(const int row, const std::uint64_t sequence) {
    beginRemoveRows({}, row, row);
    items_.erase(items_.begin() + row);
    endRemoveRows();
    emit countChanged();

    const auto key = nextStableKey_++;
    Item replacement{
        QStringLiteral("profile-%1").arg(key),
        QStringLiteral("Inserted %1").arg(key),
        sequence % 2 == 0,
        64,
        1,
    };
    beginInsertRows({}, row, row);
    items_.insert(items_.begin() + row, std::move(replacement));
    endInsertRows();
    emit countChanged();
}

void ProfileListModel::applyMutation(const std::uint64_t sequence,
                                     const QString& protectedStableKey) {
    const int row = mutableRow(sequence, protectedStableKey);
    if (row < 0) {
        return;
    }
    switch (sequence % 4) {
        case 0:
        case 1:
            updateRow(row);
            break;
        case 2:
            moveRow(row, (row + 7) % rowCount());
            break;
        case 3:
            replaceRow(row, sequence);
            break;
    }
}

bool ProfileListModel::hasUniqueStableKeys() const {
    QSet<QString> keys;
    keys.reserve(rowCount());
    for (const auto& item : items_) {
        if (item.stableKey.isEmpty() || keys.contains(item.stableKey)) {
            return false;
        }
        keys.insert(item.stableKey);
    }
    return keys.size() == rowCount();
}

}  // namespace ccs_trans::gui
