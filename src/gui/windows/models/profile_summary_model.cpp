#include "models/profile_summary_model.hpp"

#include <QString>
#include <QVariant>

#include <algorithm>

namespace ccs_trans::gui {

namespace {

QString text(const std::string& value) {
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

} // namespace

ProfileSummaryModel::ProfileSummaryModel(QObject* parent)
    : QAbstractListModel(parent) {}

int ProfileSummaryModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(items_.size());
}

QVariant ProfileSummaryModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= rowCount()) return {};
    const auto& item = items_[static_cast<std::size_t>(index.row())];
    switch (role) {
    case StableKeyRole: return QString::number(item.key);
    case ProfileIdRole: return text(item.id);
    case EnabledRole: return item.enabled;
    case ProtocolRole: return item.protocol ? text(*item.protocol) : QString{};
    case ReadinessRole: return text(item.readiness);
    case StatusDetailRole: return text(item.status_detail);
    case RuleCountRole: return QVariant::fromValue<qulonglong>(item.rule_count);
    case EnabledRuleCountRole:
        return QVariant::fromValue<qulonglong>(item.enabled_rule_count);
    default: return {};
    }
}

QHash<int, QByteArray> ProfileSummaryModel::roleNames() const {
    return {
        {StableKeyRole, "stableKey"},
        {ProfileIdRole, "profileId"},
        {EnabledRole, "profileEnabled"},
        {ProtocolRole, "profileProtocol"},
        {ReadinessRole, "readiness"},
        {StatusDetailRole, "statusDetail"},
        {RuleCountRole, "ruleCount"},
        {EnabledRuleCountRole, "enabledRuleCount"},
    };
}

int ProfileSummaryModel::count() const noexcept {
    return static_cast<int>(items_.size());
}

int ProfileSummaryModel::indexOfKey(std::int64_t key) const noexcept {
    const auto found = std::find_if(items_.cbegin(), items_.cend(), [&](const auto& item) {
        return item.key == key;
    });
    return found == items_.cend()
        ? -1 : static_cast<int>(std::distance(items_.cbegin(), found));
}

std::int64_t ProfileSummaryModel::keyAt(int row) const noexcept {
    return row >= 0 && row < rowCount()
        ? items_[static_cast<std::size_t>(row)].key : 0;
}

void ProfileSummaryModel::apply(
    const std::vector<ccs::gui_ipc::ProfileSummary>& profiles) {
    for (int row = rowCount() - 1; row >= 0; --row) {
        const auto key = items_[static_cast<std::size_t>(row)].key;
        const bool retained = std::any_of(
            profiles.cbegin(), profiles.cend(), [&](const auto& profile) {
                return profile.key == key;
            });
        if (retained) continue;
        beginRemoveRows({}, row, row);
        items_.erase(items_.begin() + row);
        endRemoveRows();
        emit countChanged();
    }

    for (int target = 0; target < static_cast<int>(profiles.size()); ++target) {
        const auto& desired = profiles[static_cast<std::size_t>(target)];
        int current = indexOfKey(desired.key);
        if (current < 0) {
            beginInsertRows({}, target, target);
            items_.insert(items_.begin() + target, desired);
            endInsertRows();
            emit countChanged();
            continue;
        }
        if (current != target) {
            moveItem(current, target);
            current = target;
        }
        updateItem(current, desired);
    }
}

void ProfileSummaryModel::clear() {
    if (items_.empty()) return;
    beginRemoveRows({}, 0, rowCount() - 1);
    items_.clear();
    endRemoveRows();
    emit countChanged();
}

void ProfileSummaryModel::moveItem(int source, int target) {
    const int destination = source < target ? target + 1 : target;
    if (!beginMoveRows({}, source, source, {}, destination)) return;
    auto moved = std::move(items_[static_cast<std::size_t>(source)]);
    items_.erase(items_.begin() + source);
    items_.insert(items_.begin() + target, std::move(moved));
    endMoveRows();
}

void ProfileSummaryModel::updateItem(
    int row,
    const ccs::gui_ipc::ProfileSummary& profile) {
    auto& current = items_[static_cast<std::size_t>(row)];
    QVector<int> changed;
    if (current.id != profile.id) changed.push_back(ProfileIdRole);
    if (current.enabled != profile.enabled) changed.push_back(EnabledRole);
    if (current.protocol != profile.protocol) changed.push_back(ProtocolRole);
    if (current.readiness != profile.readiness) changed.push_back(ReadinessRole);
    if (current.status_detail != profile.status_detail) changed.push_back(StatusDetailRole);
    if (current.rule_count != profile.rule_count) changed.push_back(RuleCountRole);
    if (current.enabled_rule_count != profile.enabled_rule_count) {
        changed.push_back(EnabledRuleCountRole);
    }
    if (changed.isEmpty()) return;
    current = profile;
    const auto model_index = index(row, 0);
    emit dataChanged(model_index, model_index, changed);
}

} // namespace ccs_trans::gui
