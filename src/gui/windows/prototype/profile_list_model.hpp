#pragma once

#include <QAbstractListModel>
#include <QString>

#include <cstdint>
#include <vector>

namespace ccs_trans::gui {

class ProfileListModel final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    enum Role {
        StableKeyRole = Qt::UserRole + 1,
        NameRole,
        EnabledRole,
        RuleCountRole,
        RevisionRole,
    };
    Q_ENUM(Role)

    explicit ProfileListModel(QObject* parent = nullptr);

    [[nodiscard]] int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;
    [[nodiscard]] int count() const noexcept;

    Q_INVOKABLE void populate(int profileCount, int rulesPerProfile);
    Q_INVOKABLE [[nodiscard]] QString stableKeyAt(int row) const;
    Q_INVOKABLE [[nodiscard]] int indexOfStableKey(const QString& stableKey) const;
    Q_INVOKABLE [[nodiscard]] bool containsStableKey(const QString& stableKey) const;
    Q_INVOKABLE [[nodiscard]] int totalRuleCount() const noexcept;

    void applyMutation(std::uint64_t sequence, const QString& protectedStableKey);
    [[nodiscard]] bool hasUniqueStableKeys() const;

signals:
    void countChanged();

private:
    struct Item {
        QString stableKey;
        QString name;
        bool enabled{false};
        int ruleCount{0};
        std::uint64_t revision{0};
    };

    [[nodiscard]] int mutableRow(std::uint64_t sequence,
                                 const QString& protectedStableKey) const;
    void updateRow(int row);
    void moveRow(int sourceRow, int targetRow);
    void replaceRow(int row, std::uint64_t sequence);

    std::vector<Item> items_;
    std::uint64_t nextStableKey_{1};
};

}  // namespace ccs_trans::gui
