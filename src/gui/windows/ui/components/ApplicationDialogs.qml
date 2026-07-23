import QtQuick
import CcsTrans.Gui

Item {
    id: root

    required property var motion
    property bool migrationPromptOffered: false
    property bool reloadPromptVisible: false
    property bool discardPromptVisible: false
    property bool localNoticeVisible: false
    property string localNoticeText
    property bool discardLocalAfterCommand: false

    function hasLocalEdits() {
        return profilesController.dirty || rulesController.dirty
                || settingsController.dirty
    }

    function inlineError() {
        var field = commandDispatcher.lastErrorField
        var command = guiState.lastCommand
        if (field === "rules") return true
        if (command === "save_profile"
                && profilesController.ownsField(field)) return true
        if (command === "update_application_fields"
                && settingsController.ownsField(field)) return true
        return commandDispatcher.lastErrorCode === "migration_required"
                || commandDispatcher.lastErrorCode
                    === "replacement_confirmation_required"
    }

    function requestReload() {
        if (hasLocalEdits() || guiState.draftDirty) {
            reloadPromptVisible = true
        } else {
            commandDispatcher.reloadDraft()
        }
    }

    function resetLocalEditors() {
        profilesController.resetLocalDraft()
        rulesController.resetLocalDraft()
        settingsController.resetLocalDraft()
    }

    function requestDiscardDraft() {
        if (hasLocalEdits()) {
            discardPromptVisible = true
            return
        }
        commandDispatcher.discardDraft()
    }

    function closeWithDiscard() {
        if (windowController.closeHasDraftEdits) {
            discardLocalAfterCommand = windowController.closeHasLocalEdits
            windowController.resolveClose("discard")
        } else {
            resetLocalEditors()
            windowController.resolveClose("discard")
        }
    }

    function closeWithApply() {
        windowController.resolveClose("apply")
    }

    function offerMigrationIfNeeded() {
        if (guiState.storageState !== "migration_required") {
            migrationPromptOffered = false
            return
        }
        if (!migrationPromptOffered) {
            migrationPromptOffered = true
            migrationController.requestMigration()
        }
    }

    Connections {
        target: guiState
        function onStorageChanged() { root.offerMigrationIfNeeded() }
    }

    Connections {
        target: windowController
        function onCloseBlocked(reason) {
            root.localNoticeText = reason
            root.localNoticeVisible = true
        }
    }

    Connections {
        target: commandDispatcher
        function onCommandFinished(command, outcome, errorCode, field, detail) {
            var succeeded = errorCode === "none"
                    && (outcome === "succeeded"
                        || outcome === "saved_pending_runtime_apply")
            if (command === "reload_draft" && succeeded) {
                root.resetLocalEditors()
                root.reloadPromptVisible = false
            }
            if (command === "discard_draft") {
                if (succeeded && root.discardLocalAfterCommand)
                    root.resetLocalEditors()
                if (succeeded || errorCode !== "none")
                    root.discardLocalAfterCommand = false
                root.discardPromptVisible = false
            }
            if (errorCode === "migration_required")
                migrationController.requestMigration()
        }
    }

    DecisionDialog {
        motion: root.motion
        visible: migrationController.migrationConfirmationRequired
        titleText: "Migrate storage"
        messageText: "ccs-trans found storage that needs an explicit migration. Continue?"
        detailText: migrationController.databasePath
        primaryText: "Yes"
        onPrimaryTriggered: migrationController.confirmMigration()
        onCancelled: migrationController.cancelMigration()
    }

    DecisionDialog {
        motion: root.motion
        visible: migrationController.replacementConfirmationRequired
        titleText: "Replace the Profile database?"
        messageText: "The existing database will be backed up before migration and replaced."
        detailText: "Database: " + migrationController.databasePath
                   + "\nBackup: " + migrationController.backupDirectory
        primaryText: "Replace"
        primaryDanger: true
        onPrimaryTriggered: migrationController.confirmReplacement()
        onCancelled: migrationController.cancelReplacement()
    }

    DecisionDialog {
        motion: root.motion
        visible: root.reloadPromptVisible
        titleText: "Reload draft"
        messageText: "Reloading will discard local editor input and the current shared draft."
        primaryText: "Reload"
        primaryDanger: true
        onPrimaryTriggered: commandDispatcher.reloadDraftDiscardingChanges()
        onCancelled: root.reloadPromptVisible = false
    }

    DecisionDialog {
        motion: root.motion
        visible: root.discardPromptVisible
        titleText: "Discard draft"
        messageText: "Discard local editor input and the current shared draft?"
        primaryText: "Discard"
        primaryDanger: true
        onPrimaryTriggered: {
            if (guiState.draftDirty) {
                root.discardLocalAfterCommand = true
                commandDispatcher.discardDraft()
            } else {
                root.resetLocalEditors()
                root.discardPromptVisible = false
            }
        }
        onCancelled: root.discardPromptVisible = false
    }

    DecisionDialog {
        motion: root.motion
        visible: windowController.closePromptVisible
        titleText: "Close ccs-trans"
        messageText: windowController.closeHasLocalEdits
                     ? "Save the editor fields before applying them, or discard them."
                     : "There are unsaved changes in the shared draft."
        primaryText: "Apply and close"
        primaryEnabled: !windowController.closeHasLocalEdits
                         && windowController.closeHasDraftEdits
        secondaryText: "Discard and close"
        onPrimaryTriggered: root.closeWithApply()
        onSecondaryTriggered: root.closeWithDiscard()
        onCancelled: windowController.resolveClose("cancel")
    }

    DecisionDialog {
        motion: root.motion
        visible: root.localNoticeVisible
        titleText: "ccs-trans"
        messageText: root.localNoticeText
        primaryText: "Dismiss"
        cancelVisible: false
        onPrimaryTriggered: root.localNoticeVisible = false
    }

    DecisionDialog {
        motion: root.motion
        visible: commandDispatcher.errorVisible && !root.inlineError()
        titleText: commandDispatcher.lastOutcome === "saved_pending_runtime_apply"
                   ? "Saved; runtime update pending" : "Command failed"
        messageText: commandDispatcher.lastErrorDetail.length > 0
                     ? commandDispatcher.lastErrorDetail
                     : commandDispatcher.lastErrorCode
        detailText: commandDispatcher.lastErrorField.length > 0
                    ? "Field: " + commandDispatcher.lastErrorField : ""
        primaryText: "Dismiss"
        cancelVisible: false
        onPrimaryTriggered: commandDispatcher.clearError()
    }

    Component.onCompleted: offerMigrationIfNeeded()
}
