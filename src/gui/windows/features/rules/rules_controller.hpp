#pragma once

#include <QObject>

#include <cstdint>

namespace ccs_trans::gui {

class CommandDispatcher;
class GuiStateStore;

class RulesController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString profileId READ profileId NOTIFY draftChanged)
    Q_PROPERTY(QString text READ text WRITE setText NOTIFY draftChanged)
    Q_PROPERTY(bool dirty READ dirty NOTIFY draftChanged)
    Q_PROPERTY(QString diagnostic READ diagnostic NOTIFY diagnosticChanged)
    Q_PROPERTY(QString error READ error NOTIFY errorChanged)

public:
    RulesController(
        GuiStateStore& state,
        CommandDispatcher& commands,
        QObject* parent = nullptr);

    [[nodiscard]] QString profileId() const;
    [[nodiscard]] QString text() const;
    [[nodiscard]] bool dirty() const noexcept;
    [[nodiscard]] QString diagnostic() const;
    [[nodiscard]] QString error() const;
    void setText(const QString& text);

    Q_INVOKABLE void save();
    Q_INVOKABLE void format();
    Q_INVOKABLE void resetLocalDraft();

signals:
    void draftChanged();
    void diagnosticChanged();
    void errorChanged();
    void textReplaced(const QString& text);

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
    QString server_text_;
    QString text_;
    QString error_;
    qulonglong submitted_draft_revision_ = 0;
    std::uint64_t edit_revision_ = 0;
    std::uint64_t submitted_edit_revision_ = 0;
    bool dirty_ = false;
    bool awaiting_server_snapshot_ = false;
};

} // namespace ccs_trans::gui
