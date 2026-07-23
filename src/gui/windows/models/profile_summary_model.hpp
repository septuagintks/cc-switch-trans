#pragma once

#include "gui_ipc/protocol_types.hpp"

#include <QAbstractListModel>

#include <cstdint>
#include <vector>

namespace ccs_trans::gui {

class ProfileSummaryModel final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    enum Role {
        StableKeyRole = Qt::UserRole + 1,
        ProfileIdRole,
        EnabledRole,
        ProtocolRole,
        ReadinessRole,
        StatusDetailRole,
        RuleCountRole,
        EnabledRuleCountRole,
    };
    Q_ENUM(Role)

    explicit ProfileSummaryModel(QObject* parent = nullptr);

    [[nodiscard]] int rowCount(
        const QModelIndex& parent = QModelIndex()) const override;
    [[nodiscard]] QVariant data(
        const QModelIndex& index,
        int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;
    [[nodiscard]] int count() const noexcept;

    Q_INVOKABLE [[nodiscard]] int indexOfKey(std::int64_t key) const noexcept;
    Q_INVOKABLE [[nodiscard]] std::int64_t keyAt(int row) const noexcept;
    void apply(const std::vector<ccs::gui_ipc::ProfileSummary>& profiles);
    void clear();

signals:
    void countChanged();

private:
    void moveItem(int source, int target);
    void updateItem(int row, const ccs::gui_ipc::ProfileSummary& profile);

    std::vector<ccs::gui_ipc::ProfileSummary> items_;
};

} // namespace ccs_trans::gui
