#include "models/editable_field_model.hpp"

#include <QStringList>

#include <algorithm>
#include <charconv>
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

QString valueText(const std::optional<ccs::gui_ipc::FieldValue>& value) {
    const auto converted = fieldValue(value);
    if (!converted.isValid()) return {};
    if (converted.metaType().id() == QMetaType::Bool) {
        return converted.toBool() ? QStringLiteral("On") : QStringLiteral("Off");
    }
    return converted.toString();
}

QString displayName(const ccs::gui_ipc::FieldState& field) {
    QString label = text(field.key);
    label.replace(QLatin1Char('.'), QLatin1Char(' '));
    label.replace(QLatin1Char('-'), QLatin1Char(' '));
    if (!label.isEmpty()) label[0] = label[0].toUpper();
    label.replace(QStringLiteral(" url"), QStringLiteral(" URL"));
    label.replace(QStringLiteral(" id"), QStringLiteral(" ID"));
    if (label == QStringLiteral("Id")) label = QStringLiteral("Profile ID");
    return label;
}

bool sameValue(
    const std::optional<ccs::gui_ipc::FieldValue>& left,
    const std::optional<ccs::gui_ipc::FieldValue>& right) {
    return left == right;
}

} // namespace

EditableFieldModel::EditableFieldModel(QObject* parent)
    : QAbstractListModel(parent) {}

int EditableFieldModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(items_.size());
}

QVariant EditableFieldModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= rowCount()) return {};
    const auto& item = items_[static_cast<std::size_t>(index.row())];
    const auto& value = item.draft;
    switch (role) {
    case KeyRole: return text(item.server.key);
    case ScopeRole: return text(item.server.scope);
    case InputKindRole: return text(item.server.input_kind);
    case RequiredRole: return item.server.required;
    case MinimumRole: return item.server.minimum
        ? QVariant::fromValue<qulonglong>(*item.server.minimum) : QVariant{};
    case MaximumRole: return item.server.maximum
        ? QVariant::fromValue<qulonglong>(*item.server.maximum) : QVariant{};
    case EnumValuesRole: {
        QStringList values;
        values.reserve(static_cast<qsizetype>(item.server.enum_values.size()));
        for (const auto& entry : item.server.enum_values) values.push_back(text(entry));
        return values;
    }
    case DisplayNameRole:
        return displayName(item.server);
    case ApplyImpactRole: return text(item.server.apply_impact);
    case ServerValueRole: return fieldValue(item.server.value);
    case DraftValueRole: return fieldValue(value);
    case ValueTextRole: return valueText(item.server.value);
    case DraftTextRole: return item.edit_text;
    case HasValueRole: return value.has_value();
    case DirtyRole: return item.locally_dirty;
    case ErrorRole: return item.error;
    default: return {};
    }
}

QHash<int, QByteArray> EditableFieldModel::roleNames() const {
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
        {ServerValueRole, "serverValue"},
        {DraftValueRole, "draftValue"},
        {ValueTextRole, "valueText"},
        {DraftTextRole, "draftText"},
        {HasValueRole, "hasValue"},
        {DirtyRole, "fieldDirty"},
        {ErrorRole, "fieldError"},
    };
}

int EditableFieldModel::count() const noexcept {
    return static_cast<int>(items_.size());
}

bool EditableFieldModel::dirty() const noexcept { return dirty_; }
bool EditableFieldModel::valid() const noexcept { return valid_; }
std::uint64_t EditableFieldModel::localRevision() const noexcept {
    return local_revision_;
}

void EditableFieldModel::apply(
    const std::vector<ccs::gui_ipc::FieldState>& fields,
    bool preserve_dirty,
    std::optional<std::uint64_t> preserve_edits_after) {
    const bool old_dirty = dirty_;
    const bool old_valid = valid_;
    for (int row = rowCount() - 1; row >= 0; --row) {
        const auto key = items_[static_cast<std::size_t>(row)].server.key;
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
        const auto& field = fields[static_cast<std::size_t>(target)];
        int current = indexOfKey(field.key);
        if (current < 0) {
            Item item;
            item.server = field;
            item.draft = field.value;
            item.edit_text = valueText(field.value);
            beginInsertRows({}, target, target);
            items_.insert(items_.begin() + target, std::move(item));
            endInsertRows();
            emit countChanged();
            continue;
        }
        if (current != target) {
            moveItem(current, target);
            current = target;
        }
        const auto& item = items_[static_cast<std::size_t>(current)];
        const bool preserve_newer_edit = preserve_edits_after
            && item.edit_revision > *preserve_edits_after;
        updateItem(current, field,
            (preserve_dirty && item.locally_dirty) || preserve_newer_edit);
    }
    dirty_ = calculateDirty();
    valid_ = calculateValid();
    emitSummaryChanges(old_dirty, old_valid);
}

void EditableFieldModel::clear() {
    if (items_.empty()) return;
    const bool old_dirty = dirty_;
    const bool old_valid = valid_;
    beginRemoveRows({}, 0, rowCount() - 1);
    items_.clear();
    endRemoveRows();
    dirty_ = false;
    valid_ = true;
    emit countChanged();
    emitSummaryChanges(old_dirty, old_valid);
}

void EditableFieldModel::discardLocal() {
    const bool old_dirty = dirty_;
    const bool old_valid = valid_;
    const auto edit_revision = ++local_revision_;
    for (int row = 0; row < rowCount(); ++row) {
        auto& item = items_[static_cast<std::size_t>(row)];
        item.draft = item.server.value;
        item.edit_text = valueText(item.server.value);
        item.locally_dirty = false;
        item.error.clear();
        item.edit_revision = edit_revision;
        const auto model_index = index(row, 0);
        emit dataChanged(model_index, model_index, {
            DraftValueRole, DraftTextRole, HasValueRole, DirtyRole, ErrorRole,
        });
    }
    dirty_ = false;
    valid_ = true;
    emitSummaryChanges(old_dirty, old_valid);
}

void EditableFieldModel::setFieldError(
    const QString& key,
    const QString& error) {
    auto* item = find(key);
    if (item == nullptr || item->error == error) return;
    item->error = error;
    const int row = indexOfKey(key.toUtf8().toStdString());
    const auto model_index = index(row, 0);
    emit dataChanged(model_index, model_index, {ErrorRole});
    const bool old_valid = valid_;
    valid_ = calculateValid();
    if (old_valid != valid_) emit validityChanged();
}

void EditableFieldModel::clearFieldErrors() {
    bool changed = false;
    for (auto& item : items_) {
        if (!item.error.isEmpty()) {
            item.error.clear();
            changed = true;
        }
    }
    if (!changed) return;
    for (int row = 0; row < rowCount(); ++row) {
        const auto model_index = index(row, 0);
        emit dataChanged(model_index, model_index, {ErrorRole});
    }
    const bool old_valid = valid_;
    valid_ = calculateValid();
    if (old_valid != valid_) emit validityChanged();
}

std::vector<ccs::gui_ipc::FieldEdit> EditableFieldModel::edits() const {
    std::vector<ccs::gui_ipc::FieldEdit> result;
    for (const auto& item : items_) {
        if (!item.locally_dirty || !item.error.isEmpty()) continue;
        result.push_back({item.server.key, item.draft});
    }
    return result;
}

QVariant EditableFieldModel::value(const QString& key) const {
    const auto* item = find(key);
    return item == nullptr ? QVariant{} : fieldValue(item->draft);
}

QString EditableFieldModel::textValue(const QString& key) const {
    const auto* item = find(key);
    return item == nullptr ? QString{} : item->edit_text;
}

bool EditableFieldModel::contains(const QString& key) const {
    return find(key) != nullptr;
}

bool EditableFieldModel::setValue(const QString& key, const QVariant& value) {
    auto* item = find(key);
    if (item == nullptr) return false;
    item->edit_revision = ++local_revision_;
    const auto source_text = value.toString();
    std::optional<ccs::gui_ipc::FieldValue> converted;
    QString error;
    if (!convertValue(*item, value, converted, error)) {
        const bool old_dirty = dirty_;
        const bool old_valid = valid_;
        item->edit_text = source_text;
        item->locally_dirty = item->edit_text != valueText(item->server.value);
        item->error = std::move(error);
        dirty_ = calculateDirty();
        valid_ = calculateValid();
        const int row = indexOfKey(item->server.key);
        const auto model_index = index(row, 0);
        emit dataChanged(model_index, model_index, {
            DraftTextRole, DirtyRole, ErrorRole,
        });
        emit fieldChanged(text(item->server.key));
        emitSummaryChanges(old_dirty, old_valid);
        return false;
    }
    item->edit_text = item->server.input_kind == "boolean"
        ? valueText(converted) : source_text;
    return setItemValue(*item, std::move(converted));
}

bool EditableFieldModel::resetValue(const QString& key) {
    auto* item = find(key);
    if (item == nullptr || item->server.required) return false;
    item->edit_revision = ++local_revision_;
    QString error;
    if (!validate(*item, std::nullopt, error)) {
        (void)setItemValue(*item, item->draft, std::move(error));
        return false;
    }
    item->edit_text.clear();
    return setItemValue(*item, std::nullopt);
}

int EditableFieldModel::indexOfKey(const std::string& key) const noexcept {
    const auto found = std::find_if(items_.cbegin(), items_.cend(), [&](const auto& item) {
        return item.server.key == key;
    });
    return found == items_.cend()
        ? -1 : static_cast<int>(std::distance(items_.cbegin(), found));
}

EditableFieldModel::Item* EditableFieldModel::find(const QString& key) noexcept {
    const auto index = indexOfKey(key.toUtf8().toStdString());
    return index < 0 ? nullptr : &items_[static_cast<std::size_t>(index)];
}

const EditableFieldModel::Item* EditableFieldModel::find(
    const QString& key) const noexcept {
    const auto index = indexOfKey(key.toUtf8().toStdString());
    return index < 0 ? nullptr : &items_[static_cast<std::size_t>(index)];
}

bool EditableFieldModel::validate(
    const Item& item,
    const std::optional<ccs::gui_ipc::FieldValue>& value,
    QString& error) const {
    error.clear();
    if (!value) {
        if (item.server.required) {
            error = QStringLiteral("This field is required.");
            return false;
        }
        return true;
    }
    if (item.server.input_kind == "boolean"
        && !std::holds_alternative<bool>(*value)) {
        error = QStringLiteral("Enter a boolean value.");
        return false;
    }
    if (item.server.input_kind == "unsigned_integer") {
        const auto* number = std::get_if<std::uint64_t>(&*value);
        if (number == nullptr) {
            error = QStringLiteral("Enter a non-negative integer.");
            return false;
        }
        if (item.server.minimum && *number < *item.server.minimum) {
            error = QStringLiteral("Value is below the supported minimum.");
            return false;
        }
        if (item.server.maximum && *number > *item.server.maximum) {
            error = QStringLiteral("Value exceeds the supported maximum.");
            return false;
        }
    }
    if (item.server.input_kind == "enumeration") {
        const auto* choice = std::get_if<std::string>(&*value);
        if (choice == nullptr || std::find(
                item.server.enum_values.cbegin(), item.server.enum_values.cend(), *choice)
                == item.server.enum_values.cend()) {
            error = QStringLiteral("Choose one of the available values.");
            return false;
        }
    }
    if (item.server.required && std::holds_alternative<std::string>(*value)
        && std::get<std::string>(*value).empty()) {
        error = QStringLiteral("This field is required.");
        return false;
    }
    return true;
}

bool EditableFieldModel::convertValue(
    const Item& item,
    const QVariant& source,
    std::optional<ccs::gui_ipc::FieldValue>& value,
    QString& error) const {
    error.clear();
    const auto input = item.server.input_kind;
    if ((input == "text" || input == "path" || input == "url"
            || input == "enumeration") && source.toString().isEmpty()
        && !item.server.required) {
        value.reset();
        return true;
    }
    if (input == "boolean") {
        if (source.metaType().id() == QMetaType::Bool) {
            value = source.toBool();
        } else {
            const auto raw = source.toString().trimmed();
            if (raw == "true") value = true;
            else if (raw == "false") value = false;
            else {
                error = QStringLiteral("Enter a boolean value.");
                return false;
            }
        }
    } else if (input == "unsigned_integer") {
        const auto raw = source.toString().trimmed();
        bool ok = false;
        const auto number = raw.toULongLong(&ok);
        if (!ok || raw.isEmpty() || raw.startsWith(QLatin1Char('-'))) {
            error = QStringLiteral("Enter a non-negative integer.");
            return false;
        }
        value = static_cast<std::uint64_t>(number);
    } else {
        value = source.toString().toUtf8().toStdString();
    }
    if (!validate(item, value, error)) return false;
    return true;
}

bool EditableFieldModel::setItemValue(
    Item& item,
    std::optional<ccs::gui_ipc::FieldValue> value,
    QString error) {
    const bool old_dirty = dirty_;
    const bool old_valid = valid_;
    const bool accepted = error.isEmpty();
    if (!accepted) {
        item.error = std::move(error);
    } else {
        item.error.clear();
        item.locally_dirty = !sameValue(item.server.value, value);
        item.draft = std::move(value);
        if (!item.locally_dirty) {
            item.edit_text = valueText(item.server.value);
        }
    }
    dirty_ = calculateDirty();
    valid_ = calculateValid();
    const int row = indexOfKey(item.server.key);
    const auto model_index = index(row, 0);
    emit dataChanged(model_index, model_index, {
        DraftValueRole, DraftTextRole, HasValueRole, DirtyRole, ErrorRole,
    });
    emit fieldChanged(text(item.server.key));
    emitSummaryChanges(old_dirty, old_valid);
    return accepted;
}

bool EditableFieldModel::calculateDirty() const noexcept {
    return std::any_of(items_.cbegin(), items_.cend(), [](const auto& item) {
        return item.locally_dirty;
    });
}

bool EditableFieldModel::calculateValid() const noexcept {
    return std::all_of(items_.cbegin(), items_.cend(), [](const auto& item) {
        return item.error.isEmpty();
    });
}

void EditableFieldModel::moveItem(int source, int target) {
    const int destination = source < target ? target + 1 : target;
    if (!beginMoveRows({}, source, source, {}, destination)) return;
    auto moved = std::move(items_[static_cast<std::size_t>(source)]);
    items_.erase(items_.begin() + source);
    items_.insert(items_.begin() + target, std::move(moved));
    endMoveRows();
}

void EditableFieldModel::updateItem(
    int row,
    const ccs::gui_ipc::FieldState& field,
    bool preserve_local) {
    auto& item = items_[static_cast<std::size_t>(row)];
    const auto old_server = item.server;
    const auto old_draft = item.draft;
    const auto old_edit_text = item.edit_text;
    const bool old_dirty = item.locally_dirty;
    const auto old_error = item.error;
    item.server = field;
    if (!preserve_local) {
        item.draft = field.value;
        item.edit_text = valueText(field.value);
        item.locally_dirty = false;
        item.error.clear();
    } else if (item.error.isEmpty()) {
        item.locally_dirty = !sameValue(item.draft, field.value);
        if (!item.locally_dirty) item.edit_text = valueText(field.value);
    } else {
        item.locally_dirty = true;
    }
    QVector<int> changed;
    if (old_server.scope != field.scope) changed.push_back(ScopeRole);
    if (old_server.input_kind != field.input_kind) changed.push_back(InputKindRole);
    if (old_server.required != field.required) changed.push_back(RequiredRole);
    if (old_server.minimum != field.minimum) changed.push_back(MinimumRole);
    if (old_server.maximum != field.maximum) changed.push_back(MaximumRole);
    if (old_server.enum_values != field.enum_values) changed.push_back(EnumValuesRole);
    if (old_server.display_name_key != field.display_name_key) {
        changed.push_back(DisplayNameRole);
    }
    if (old_server.apply_impact != field.apply_impact) changed.push_back(ApplyImpactRole);
    if (!sameValue(old_server.value, field.value)) {
        changed.push_back(ServerValueRole);
        changed.push_back(ValueTextRole);
    }
    if (!sameValue(old_draft, item.draft)) {
        changed.push_back(DraftValueRole);
        changed.push_back(HasValueRole);
    }
    if (old_edit_text != item.edit_text) changed.push_back(DraftTextRole);
    if (old_dirty != item.locally_dirty) changed.push_back(DirtyRole);
    if (old_error != item.error) changed.push_back(ErrorRole);
    if (changed.isEmpty()) return;
    const auto model_index = index(row, 0);
    emit dataChanged(model_index, model_index, changed);
}

void EditableFieldModel::emitSummaryChanges(bool old_dirty, bool old_valid) {
    if (old_dirty != dirty_) emit dirtyChanged();
    if (old_valid != valid_) emit validityChanged();
}

} // namespace ccs_trans::gui
