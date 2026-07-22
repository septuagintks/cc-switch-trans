#include "models/configuration_field_model.hpp"

#include <QStringList>
#include <QVariant>

#include <algorithm>
#include <type_traits>

namespace ccs_trans::gui {

namespace {

QString text(const std::string& value) {
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

QVariant fieldValue(const std::optional<ccs::gui_ipc::FieldValue>& value) {
    if (!value) return {};
    return std::visit([](const auto& item) -> QVariant {
        using Value = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<Value, std::string>) {
            return text(item);
        } else if constexpr (std::is_same_v<Value, std::uint64_t>) {
            return QVariant::fromValue<qulonglong>(item);
        } else {
            return item;
        }
    }, *value);
}

QString fieldValueText(const std::optional<ccs::gui_ipc::FieldValue>& value) {
    const auto converted = fieldValue(value);
    if (!converted.isValid()) return QStringLiteral("Not set");
    if (converted.metaType().id() == QMetaType::Bool) {
        return converted.toBool() ? QStringLiteral("On") : QStringLiteral("Off");
    }
    return converted.toString();
}

} // namespace

ConfigurationFieldModel::ConfigurationFieldModel(QObject* parent)
    : QAbstractListModel(parent) {}

int ConfigurationFieldModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(items_.size());
}

QVariant ConfigurationFieldModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= rowCount()) return {};
    const auto& item = items_[static_cast<std::size_t>(index.row())];
    switch (role) {
    case KeyRole: return text(item.key);
    case ScopeRole: return text(item.scope);
    case InputKindRole: return text(item.input_kind);
    case RequiredRole: return item.required;
    case MinimumRole: return item.minimum
        ? QVariant::fromValue<qulonglong>(*item.minimum) : QVariant{};
    case MaximumRole: return item.maximum
        ? QVariant::fromValue<qulonglong>(*item.maximum) : QVariant{};
    case EnumValuesRole: {
        QStringList values;
        values.reserve(static_cast<qsizetype>(item.enum_values.size()));
        for (const auto& value : item.enum_values) values.push_back(text(value));
        return values;
    }
    case DisplayNameRole:
        return item.display_name_key.empty() ? text(item.key) : text(item.display_name_key);
    case ApplyImpactRole: return text(item.apply_impact);
    case ValueRole: return fieldValue(item.value);
    case ValueTextRole: return fieldValueText(item.value);
    default: return {};
    }
}

QHash<int, QByteArray> ConfigurationFieldModel::roleNames() const {
    return {
        {KeyRole, "fieldKey"},
        {ScopeRole, "fieldScope"},
        {InputKindRole, "inputKind"},
        {RequiredRole, "fieldRequired"},
        {MinimumRole, "minimumValue"},
        {MaximumRole, "maximumValue"},
        {EnumValuesRole, "enumValues"},
        {DisplayNameRole, "displayName"},
        {ApplyImpactRole, "applyImpact"},
        {ValueRole, "fieldValue"},
        {ValueTextRole, "valueText"},
    };
}

int ConfigurationFieldModel::count() const noexcept {
    return static_cast<int>(items_.size());
}

void ConfigurationFieldModel::apply(
    const std::vector<ccs::gui_ipc::FieldState>& fields) {
    for (int row = rowCount() - 1; row >= 0; --row) {
        const auto key = items_[static_cast<std::size_t>(row)].key;
        if (std::any_of(fields.cbegin(), fields.cend(), [&](const auto& field) {
                return field.key == key;
            })) {
            continue;
        }
        beginRemoveRows({}, row, row);
        items_.erase(items_.begin() + row);
        endRemoveRows();
        emit countChanged();
    }
    for (int target = 0; target < static_cast<int>(fields.size()); ++target) {
        const auto& desired = fields[static_cast<std::size_t>(target)];
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

void ConfigurationFieldModel::clear() {
    if (items_.empty()) return;
    beginRemoveRows({}, 0, rowCount() - 1);
    items_.clear();
    endRemoveRows();
    emit countChanged();
}

int ConfigurationFieldModel::indexOfKey(const std::string& key) const noexcept {
    const auto found = std::find_if(items_.cbegin(), items_.cend(), [&](const auto& item) {
        return item.key == key;
    });
    return found == items_.cend()
        ? -1 : static_cast<int>(std::distance(items_.cbegin(), found));
}

void ConfigurationFieldModel::moveItem(int source, int target) {
    const int destination = source < target ? target + 1 : target;
    if (!beginMoveRows({}, source, source, {}, destination)) return;
    auto moved = std::move(items_[static_cast<std::size_t>(source)]);
    items_.erase(items_.begin() + source);
    items_.insert(items_.begin() + target, std::move(moved));
    endMoveRows();
}

void ConfigurationFieldModel::updateItem(
    int row,
    const ccs::gui_ipc::FieldState& field) {
    auto& current = items_[static_cast<std::size_t>(row)];
    if (current == field) return;
    current = field;
    const auto model_index = index(row, 0);
    emit dataChanged(model_index, model_index, {
        ScopeRole,
        InputKindRole,
        RequiredRole,
        MinimumRole,
        MaximumRole,
        EnumValuesRole,
        DisplayNameRole,
        ApplyImpactRole,
        ValueRole,
        ValueTextRole,
    });
}

} // namespace ccs_trans::gui
