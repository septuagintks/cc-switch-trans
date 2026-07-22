#pragma once

#include <QObject>

namespace ccs_trans::gui {

class CommandDispatcher;
class GuiStateStore;

class RulesController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString profileId READ profileId NOTIFY draftChanged)
    Q_PROPERTY(QString text READ text WRITE setText NOTIFY draftChanged)
    Q_PROPERTY(bool dirty READ dirty NOTIFY draftChanged)
    Q_PROPERTY(QString diagnostic READ diagnostic NOTIFY diagnosticChanged)

public:
    RulesController(
        GuiStateStore& state,
        CommandDispatcher& commands,
        QObject* parent = nullptr);

    [[nodiscard]] QString profileId() const;
    [[nodiscard]] QString text() const;
    [[nodiscard]] bool dirty() const noexcept;
    [[nodiscard]] QString diagnostic() const;
    void setText(const QString& text);

    Q_INVOKABLE void save();
    Q_INVOKABLE void format();
    Q_INVOKABLE void resetLocalDraft();

signals:
    void draftChanged();
    void diagnosticChanged();

private slots:
    void syncFromState();
    void handleCommandFinished(
        const QString& command,
        const QString& outcome,
        const QString& errorCode,
        const QString& field,
        const QString& detail);

private:
    void submit(bool format);

    GuiStateStore& state_;
    CommandDispatcher& commands_;
    QString profile_key_;
    QString profile_id_;
    QString text_;
    bool dirty_ = false;
};

} // namespace ccs_trans::gui
