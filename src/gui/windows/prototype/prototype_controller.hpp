#pragma once

#include <QObject>
#include <QTimer>

#include <cstdint>

namespace ccs_trans::gui {

class ProfileListModel;

class PrototypeController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString selectedKey READ selectedKey WRITE setSelectedKey NOTIFY selectedKeyChanged)
    Q_PROPERTY(int selectedIndex READ selectedIndex NOTIFY selectedIndexChanged)
    Q_PROPERTY(bool stressRunning READ stressRunning NOTIFY stressRunningChanged)
    Q_PROPERTY(int completedMutations READ completedMutations NOTIFY completedMutationsChanged)

public:
    explicit PrototypeController(ProfileListModel* model, QObject* parent = nullptr);

    [[nodiscard]] QString selectedKey() const;
    [[nodiscard]] int selectedIndex() const;
    [[nodiscard]] bool stressRunning() const noexcept;
    [[nodiscard]] int completedMutations() const noexcept;

    Q_INVOKABLE void setSelectedKey(const QString& selectedKey);
    Q_INVOKABLE void startStress(int mutationCount);

signals:
    void selectedKeyChanged();
    void selectedIndexChanged();
    void stressRunningChanged();
    void completedMutationsChanged();
    void stressFinished(qint64 elapsedMilliseconds);

private slots:
    void runStressBatch();
    void refreshSelectionIndex();

private:
    ProfileListModel* model_;
    QTimer stressTimer_;
    QString selectedKey_;
    int remainingMutations_{0};
    int completedMutations_{0};
    std::uint64_t mutationSequence_{0};
    qint64 stressStartedAt_{0};
};

}  // namespace ccs_trans::gui
