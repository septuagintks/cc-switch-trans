#pragma once

#include "models/editable_field_model.hpp"

#include <QObject>
#include <QVariant>

#include <cstdint>

namespace ccs_trans::gui {

class CommandDispatcher;
class GuiStateStore;

class ProfilesController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString profileKey READ profileKey NOTIFY draftChanged)
    Q_PROPERTY(QString profileId READ profileId WRITE setProfileId NOTIFY draftChanged)
    Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY draftChanged)
    Q_PROPERTY(bool dirty READ dirty NOTIFY draftChanged)
    Q_PROPERTY(bool valid READ valid NOTIFY draftChanged)
    Q_PROPERTY(QObject* fieldsModel READ fieldsModel CONSTANT)

public:
    ProfilesController(
        GuiStateStore& state,
        CommandDispatcher& commands,
        QObject* parent = nullptr);

    [[nodiscard]] QString profileKey() const;
    [[nodiscard]] QString profileId() const;
    [[nodiscard]] bool enabled() const noexcept;
    [[nodiscard]] bool dirty() const noexcept;
    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] QObject* fieldsModel() noexcept;

    void setProfileId(const QString& profile_id);
    void setEnabled(bool enabled);

    Q_INVOKABLE void selectProfile(const QString& stable_key);
    Q_INVOKABLE void save();
    Q_INVOKABLE void createProfile(const QString& profile_id);
    Q_INVOKABLE void removeSelected();
    Q_INVOKABLE void moveSelected(int position);
    Q_INVOKABLE bool setFieldValue(const QString& key, const QVariant& value);
    Q_INVOKABLE bool resetFieldValue(const QString& key);
    Q_INVOKABLE bool ownsField(const QString& key) const;
    Q_INVOKABLE void resetLocalDraft();

signals:
    void draftChanged();

private slots:
    void syncFromState();
    void handleFieldChanged(const QString& key);
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
    qulonglong submitted_draft_revision_ = 0;
    std::uint64_t submitted_local_revision_ = 0;
    bool awaiting_server_snapshot_ = false;
    EditableFieldModel fields_;
};

} // namespace ccs_trans::gui
