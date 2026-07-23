#pragma once

#include "gui_ipc/protocol_types.hpp"

#include <QAbstractListModel>

#include <cstdint>
#include <optional>
#include <vector>

namespace ccs_trans::gui {

class EditableFieldModel final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    Q_PROPERTY(bool dirty READ dirty NOTIFY dirtyChanged)
    Q_PROPERTY(bool valid READ valid NOTIFY validityChanged)

public:
    enum Role {
        KeyRole = Qt::UserRole + 1,
        ScopeRole,
        InputKindRole,
        RequiredRole,
        MinimumRole,
        MaximumRole,
        EnumValuesRole,
        DisplayNameRole,
        ApplyImpactRole,
        ServerValueRole,
        DraftValueRole,
        ValueTextRole,
        DraftTextRole,
        HasValueRole,
        DirtyRole,
        ErrorRole,
    };
    Q_ENUM(Role)

    explicit EditableFieldModel(QObject* parent = nullptr);

    [[nodiscard]] int rowCount(
        const QModelIndex& parent = QModelIndex()) const override;
    [[nodiscard]] QVariant data(
        const QModelIndex& index,
        int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;
    [[nodiscard]] int count() const noexcept;
    [[nodiscard]] bool dirty() const noexcept;
    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] std::uint64_t localRevision() const noexcept;

    void apply(
        const std::vector<ccs::gui_ipc::FieldState>& fields,
        bool preserve_dirty = true,
        std::optional<std::uint64_t> preserve_edits_after = std::nullopt);
    void clear();
    void discardLocal();
    void setFieldError(const QString& key, const QString& error);
    void clearFieldErrors();

    [[nodiscard]] std::vector<ccs::gui_ipc::FieldEdit> edits() const;
    [[nodiscard]] QVariant value(const QString& key) const;
    [[nodiscard]] QString textValue(const QString& key) const;
    [[nodiscard]] bool contains(const QString& key) const;

    Q_INVOKABLE bool setValue(const QString& key, const QVariant& value);
    Q_INVOKABLE bool resetValue(const QString& key);

signals:
    void countChanged();
    void dirtyChanged();
    void validityChanged();
    void fieldChanged(const QString& key);

private:
    struct Item {
        ccs::gui_ipc::FieldState server;
        std::optional<ccs::gui_ipc::FieldValue> draft;
        QString edit_text;
        bool locally_dirty = false;
        QString error;
        std::uint64_t edit_revision = 0;
    };

    [[nodiscard]] int indexOfKey(const std::string& key) const noexcept;
    [[nodiscard]] Item* find(const QString& key) noexcept;
    [[nodiscard]] const Item* find(const QString& key) const noexcept;
    [[nodiscard]] bool validate(
        const Item& item,
        const std::optional<ccs::gui_ipc::FieldValue>& value,
        QString& error) const;
    [[nodiscard]] bool convertValue(
        const Item& item,
        const QVariant& source,
        std::optional<ccs::gui_ipc::FieldValue>& value,
        QString& error) const;
    [[nodiscard]] bool setItemValue(
        Item& item,
        std::optional<ccs::gui_ipc::FieldValue> value,
        QString error = {});
    [[nodiscard]] bool calculateDirty() const noexcept;
    [[nodiscard]] bool calculateValid() const noexcept;
    void moveItem(int source, int target);
    void updateItem(
        int row,
        const ccs::gui_ipc::FieldState& field,
        bool preserve_local);
    void emitSummaryChanges(bool old_dirty, bool old_valid);

    std::vector<Item> items_;
    bool dirty_ = false;
    bool valid_ = true;
    std::uint64_t local_revision_ = 0;
};

} // namespace ccs_trans::gui
