#pragma once

#include "models/editable_field_model.hpp"

#include <QObject>
#include <QVariant>

#include <cstdint>

namespace ccs_trans::gui {

class CommandDispatcher;
class GuiStateStore;

class SettingsController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool lightweightMode READ lightweightMode NOTIFY lightweightModeChanged)
    Q_PROPERTY(bool dirty READ dirty NOTIFY draftChanged)
    Q_PROPERTY(bool valid READ valid NOTIFY draftChanged)
    Q_PROPERTY(QObject* fieldsModel READ fieldsModel CONSTANT)

public:
    SettingsController(
        GuiStateStore& state,
        CommandDispatcher& commands,
        QObject* parent = nullptr);

    [[nodiscard]] bool lightweightMode() const noexcept;
    [[nodiscard]] bool dirty() const noexcept;
    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] QObject* fieldsModel() noexcept;
    Q_INVOKABLE void setLightweightMode(bool enabled);
    Q_INVOKABLE bool setFieldValue(const QString& key, const QVariant& value);
    Q_INVOKABLE bool resetFieldValue(const QString& key);
    Q_INVOKABLE bool ownsField(const QString& key) const;
    Q_INVOKABLE void save();
    Q_INVOKABLE void resetLocalDraft();

signals:
    void lightweightModeChanged();
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
    qulonglong submitted_draft_revision_ = 0;
    std::uint64_t submitted_local_revision_ = 0;
    bool awaiting_server_snapshot_ = false;
    EditableFieldModel fields_;
};

} // namespace ccs_trans::gui
