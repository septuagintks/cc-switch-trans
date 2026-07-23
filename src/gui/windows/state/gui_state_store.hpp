#pragma once

#include "gui_ipc/protocol_types.hpp"
#include "models/configuration_field_model.hpp"
#include "models/profile_summary_model.hpp"

#include <QObject>

#include <optional>

namespace ccs_trans::gui {

class GuiIpcClient;

class GuiStateStore final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QObject* profilesModel READ profilesModel CONSTANT)
    Q_PROPERTY(QObject* profileFieldsModel READ profileFieldsModel CONSTANT)
    Q_PROPERTY(QObject* applicationFieldsModel READ applicationFieldsModel CONSTANT)
    Q_PROPERTY(qulonglong revision READ revision NOTIFY revisionChanged)
    Q_PROPERTY(QString applicationState READ applicationState NOTIFY applicationChanged)
    Q_PROPERTY(QString listenerAddress READ listenerAddress NOTIFY applicationChanged)
    Q_PROPERTY(QString applicationError READ applicationError NOTIFY applicationChanged)
    Q_PROPERTY(bool canStart READ canStart NOTIFY applicationChanged)
    Q_PROPERTY(bool canStop READ canStop NOTIFY applicationChanged)
    Q_PROPERTY(bool canReload READ canReload NOTIFY applicationChanged)
    Q_PROPERTY(QString selectedProfileKey READ selectedProfileKey NOTIFY selectionChanged)
    Q_PROPERTY(QString selectedProfileId READ selectedProfileId NOTIFY selectionChanged)
    Q_PROPERTY(bool selectedProfileEnabled READ selectedProfileEnabled NOTIFY selectionChanged)
    Q_PROPERTY(qulonglong selectedRuleCount READ selectedRuleCount NOTIFY selectionChanged)
    Q_PROPERTY(qulonglong selectedEnabledRuleCount READ selectedEnabledRuleCount NOTIFY selectionChanged)
    Q_PROPERTY(QString rulesText READ rulesText NOTIFY editorChanged)
    Q_PROPERTY(QString rulesDiagnostic READ rulesDiagnostic NOTIFY editorChanged)
    Q_PROPERTY(QString draftPhase READ draftPhase NOTIFY draftChanged)
    Q_PROPERTY(qulonglong draftRevision READ draftRevision NOTIFY draftChanged)
    Q_PROPERTY(QString baseRevision READ baseRevision NOTIFY draftChanged)
    Q_PROPERTY(bool draftDirty READ draftDirty NOTIFY draftChanged)
    Q_PROPERTY(bool runtimeApplyPending READ runtimeApplyPending NOTIFY draftChanged)
    Q_PROPERTY(bool commandPending READ commandPending NOTIFY commandStateChanged)
    Q_PROPERTY(QString lastCommand READ lastCommand NOTIFY commandStateChanged)
    Q_PROPERTY(QString lastCommandOutcome READ lastCommandOutcome NOTIFY commandStateChanged)
    Q_PROPERTY(QString lastCommandError READ lastCommandError NOTIFY commandStateChanged)
    Q_PROPERTY(QString lastCommandField READ lastCommandField NOTIFY commandStateChanged)
    Q_PROPERTY(QString lastCommandDetail READ lastCommandDetail NOTIFY commandStateChanged)
    Q_PROPERTY(QString storageState READ storageState NOTIFY storageChanged)
    Q_PROPERTY(bool storageDatabaseExists READ storageDatabaseExists NOTIFY storageChanged)
    Q_PROPERTY(QString storageDetail READ storageDetail NOTIFY storageChanged)
    Q_PROPERTY(QString storageDatabasePath READ storageDatabasePath NOTIFY storageChanged)
    Q_PROPERTY(QString storageBackupDirectory READ storageBackupDirectory NOTIFY storageChanged)
    Q_PROPERTY(bool lightweightMode READ lightweightMode NOTIFY preferenceChanged)

public:
    explicit GuiStateStore(GuiIpcClient& client, QObject* parent = nullptr);

    [[nodiscard]] QObject* profilesModel() noexcept;
    [[nodiscard]] QObject* profileFieldsModel() noexcept;
    [[nodiscard]] QObject* applicationFieldsModel() noexcept;
    [[nodiscard]] qulonglong revision() const noexcept;
    [[nodiscard]] QString applicationState() const;
    [[nodiscard]] QString listenerAddress() const;
    [[nodiscard]] QString applicationError() const;
    [[nodiscard]] bool canStart() const;
    [[nodiscard]] bool canStop() const;
    [[nodiscard]] bool canReload() const;
    [[nodiscard]] QString selectedProfileKey() const;
    [[nodiscard]] QString selectedProfileId() const;
    [[nodiscard]] bool selectedProfileEnabled() const;
    [[nodiscard]] qulonglong selectedRuleCount() const noexcept;
    [[nodiscard]] qulonglong selectedEnabledRuleCount() const noexcept;
    [[nodiscard]] QString rulesText() const;
    [[nodiscard]] QString rulesDiagnostic() const;
    [[nodiscard]] QString draftPhase() const;
    [[nodiscard]] qulonglong draftRevision() const noexcept;
    [[nodiscard]] QString baseRevision() const;
    [[nodiscard]] bool draftDirty() const;
    [[nodiscard]] bool runtimeApplyPending() const noexcept;
    [[nodiscard]] bool commandPending() const noexcept;
    [[nodiscard]] QString lastCommand() const;
    [[nodiscard]] QString lastCommandOutcome() const;
    [[nodiscard]] QString lastCommandError() const;
    [[nodiscard]] QString lastCommandField() const;
    [[nodiscard]] QString lastCommandDetail() const;
    [[nodiscard]] QString storageState() const;
    [[nodiscard]] bool storageDatabaseExists() const noexcept;
    [[nodiscard]] QString storageDetail() const;
    [[nodiscard]] QString storageDatabasePath() const;
    [[nodiscard]] QString storageBackupDirectory() const;
    [[nodiscard]] bool lightweightMode() const noexcept;
    [[nodiscard]] const std::optional<ccs::gui_ipc::Snapshot>& snapshot() const noexcept;

signals:
    void revisionChanged();
    void applicationChanged();
    void selectionChanged();
    void editorChanged();
    void draftChanged();
    void commandStateChanged();
    void storageChanged();
    void preferenceChanged();
    void snapshotApplied();

private slots:
    void applyClientSnapshot();
    void applyCommandStatus();

private:
    static QString text(const std::string& value);
    const ccs::gui_ipc::ProfileSummary* selectedProfile() const noexcept;
    void emitSnapshotDifferences(
        const std::optional<ccs::gui_ipc::Snapshot>& previous);

    GuiIpcClient& client_;
    ProfileSummaryModel profiles_;
    ConfigurationFieldModel profile_fields_;
    ConfigurationFieldModel application_fields_;
    std::optional<ccs::gui_ipc::Snapshot> snapshot_;
    std::optional<ccs::gui_ipc::CommandStatus> last_command_;
};

} // namespace ccs_trans::gui
