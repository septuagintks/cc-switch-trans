#pragma once

#include "gui_ipc/protocol_types.hpp"

#include <QAbstractListModel>

#include <vector>

namespace ccs_trans::gui {

class ConfigurationFieldModel final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)

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
        ValueRole,
        ValueTextRole,
    };
    Q_ENUM(Role)

    explicit ConfigurationFieldModel(QObject* parent = nullptr);

    [[nodiscard]] int rowCount(
        const QModelIndex& parent = QModelIndex()) const override;
    [[nodiscard]] QVariant data(
        const QModelIndex& index,
        int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;
    [[nodiscard]] int count() const noexcept;

    void apply(const std::vector<ccs::gui_ipc::FieldState>& fields);
    void clear();

signals:
    void countChanged();

private:
    [[nodiscard]] int indexOfKey(const std::string& key) const noexcept;
    void moveItem(int source, int target);
    void updateItem(int row, const ccs::gui_ipc::FieldState& field);

    std::vector<ccs::gui_ipc::FieldState> items_;
};

} // namespace ccs_trans::gui
