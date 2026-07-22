#pragma once

#include <QObject>

namespace ccs_trans::gui {

class CommandDispatcher;
class GuiStateStore;

class ProfilesController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString profileKey READ profileKey NOTIFY draftChanged)
    Q_PROPERTY(QString profileId READ profileId WRITE setProfileId NOTIFY draftChanged)
    Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY draftChanged)
    Q_PROPERTY(bool dirty READ dirty NOTIFY draftChanged)

public:
    ProfilesController(
        GuiStateStore& state,
        CommandDispatcher& commands,
        QObject* parent = nullptr);

    [[nodiscard]] QString profileKey() const;
    [[nodiscard]] QString profileId() const;
    [[nodiscard]] bool enabled() const noexcept;
    [[nodiscard]] bool dirty() const noexcept;

    void setProfileId(const QString& profile_id);
    void setEnabled(bool enabled);

    Q_INVOKABLE void selectProfile(const QString& stable_key);
    Q_INVOKABLE void save();
    Q_INVOKABLE void createProfile(const QString& profile_id);
    Q_INVOKABLE void removeSelected();
    Q_INVOKABLE void resetLocalDraft();

signals:
    void draftChanged();

private slots:
    void syncFromState();
    void handleCommandFinished(
        const QString& command,
        const QString& outcome,
        const QString& errorCode,
        const QString& field,
        const QString& detail);

private:
    GuiStateStore& state_;
    CommandDispatcher& commands_;
    QString profile_key_;
    QString profile_id_;
    bool enabled_ = false;
    bool dirty_ = false;
};

} // namespace ccs_trans::gui
