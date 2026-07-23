#include "controllers/command_dispatcher.hpp"
#include "features/migration/migration_controller.hpp"
#include "features/profiles/profiles_controller.hpp"
#include "features/rules/rules_controller.hpp"
#include "features/settings/settings_controller.hpp"
#include "gui_ipc/frame_codec.hpp"
#include "gui_ipc/json_codec.hpp"
#include "ipc/gui_ipc_client.hpp"
#include "lifecycle/gui_window_controller.hpp"
#include "state/gui_state_store.hpp"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QSignalSpy>
#include <QTest>
#include <QVariant>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#ifdef StartService
#undef StartService
#endif

#include <array>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
constexpr std::string_view kSessionId = "feature-controller-session";

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

std::string read_frame(HANDLE pipe) {
    ccs::gui_ipc::FrameDecoder decoder;
    std::array<std::uint8_t, 4096> buffer{};
    std::vector<std::string> frames;
    while (frames.empty()) {
        DWORD received = 0;
        require(ReadFile(
                    pipe,
                    buffer.data(),
                    static_cast<DWORD>(buffer.size()),
                    &received,
                    nullptr)
                && received > 0,
            "fake tray failed to read a frame");
        ccs::gui_ipc::FrameError frame_error;
        require(decoder.consume(
                    std::span<const std::uint8_t>(buffer.data(), received),
                    frames,
                    frame_error),
            "fake tray rejected a frame");
    }
    require(frames.size() == 1 && decoder.buffered_bytes() == 0,
        "fake tray expected one complete frame");
    return std::move(frames.front());
}

void write_frame(HANDLE pipe, const ccs::gui_ipc::Envelope& envelope) {
    std::string payload;
    std::string error;
    require(ccs::gui_ipc::serialize_envelope(envelope, payload, error), error);
    std::vector<std::uint8_t> frame;
    ccs::gui_ipc::FrameError frame_error;
    require(ccs::gui_ipc::encode_frame(payload, frame, frame_error),
        "fake tray failed to encode a frame");
    std::size_t offset = 0;
    while (offset < frame.size()) {
        DWORD written = 0;
        require(WriteFile(
                    pipe,
                    frame.data() + offset,
                    static_cast<DWORD>(frame.size() - offset),
                    &written,
                    nullptr)
                && written > 0,
            "fake tray failed to write a frame");
        offset += written;
    }
}

ccs::gui_ipc::Envelope server_envelope(
    ccs::gui_ipc::MessageKind kind,
    std::uint64_t sequence,
    const ccs::gui_ipc::Hello& hello,
    std::string request_id = {}) {
    ccs::gui_ipc::Envelope envelope;
    envelope.kind = kind;
    envelope.request_id = std::move(request_id);
    envelope.session_id = kSessionId;
    envelope.source_commit = hello.source_commit;
    envelope.sequence = sequence;
    return envelope;
}

ccs::gui_ipc::Snapshot fixture_snapshot() {
    ccs::gui_ipc::Snapshot snapshot;
    snapshot.revision = 1;
    snapshot.application = {"stopped", "127.0.0.1", 15723, {}, 0};
    snapshot.profiles = {
        {1, "alpha", true, std::string{"responses"}, "ready", {}, 2, 1},
        {2, "beta", false, std::string{"chat-completions"}, "ready", {}, 1, 0},
    };
    snapshot.selection = {std::string{"beta"}, 2};
    snapshot.profile_editor = ccs::gui_ipc::ProfileEditor{
        2,
        "beta",
        {
            {"id", "profile", "text", true, {}, {}, {},
                "field.profile.id", "runtime_reload", std::string{"beta"}},
            {"enabled", "profile", "boolean", true, {}, {}, {},
                "field.profile.enabled", "runtime_reload", false},
        },
    };
    snapshot.rules_editor = ccs::gui_ipc::RulesEditor{2, "beta", "[]\n", {}};
    snapshot.application_fields = {
        {"listener.port", "application", "unsigned_integer", true,
            1, 65'535, {}, "field.listener.port", "service_restart",
            std::uint64_t{15'723}},
    };
    snapshot.draft = {"dirty", false, 3, "base-revision"};
    snapshot.storage = {
        "migration_required",
        true,
        "legacy storage requires migration",
        "C:/Users/test/.ccs-trans/profiles.db",
        "C:/Users/test/.ccs-trans/migrations",
    };
    snapshot.lightweight_mode = false;
    return snapshot;
}

void apply_field_edits(
    std::vector<ccs::gui_ipc::FieldState>& fields,
    const std::vector<ccs::gui_ipc::FieldEdit>& edits) {
    for (const auto& edit : edits) {
        const auto field = std::find_if(
            fields.begin(), fields.end(), [&](const auto& candidate) {
                return candidate.key == edit.key;
            });
        if (field != fields.end()) field->value = edit.value;
    }
}

std::string_view completion_command_name(ccs::gui_ipc::GuiCommand command) {
    switch (command) {
    case ccs::gui_ipc::GuiCommand::Refresh: return "refresh";
    case ccs::gui_ipc::GuiCommand::StartService: return "start_service";
    case ccs::gui_ipc::GuiCommand::StopService: return "stop_service";
    case ccs::gui_ipc::GuiCommand::ReloadService: return "reload_service";
    case ccs::gui_ipc::GuiCommand::QuitApplication: return "quit_application";
    case ccs::gui_ipc::GuiCommand::ApplyDraft: return "apply_draft";
    case ccs::gui_ipc::GuiCommand::DiscardDraft: return "discard_draft";
    case ccs::gui_ipc::GuiCommand::ReloadDraft: return "reload_draft";
    case ccs::gui_ipc::GuiCommand::SelectProfile: return "select_profile";
    case ccs::gui_ipc::GuiCommand::CreateProfile: return "create_profile";
    case ccs::gui_ipc::GuiCommand::RemoveProfile: return "remove_profile";
    case ccs::gui_ipc::GuiCommand::SaveProfile: return "save_profile";
    case ccs::gui_ipc::GuiCommand::SetProfileEnabled:
        return "set_profile_enabled";
    case ccs::gui_ipc::GuiCommand::MoveProfile: return "move_profile";
    case ccs::gui_ipc::GuiCommand::ReplaceRulesText:
        return "replace_rules_text";
    case ccs::gui_ipc::GuiCommand::FormatRulesText: return "format_rules_text";
    case ccs::gui_ipc::GuiCommand::UpdateApplicationFields:
        return "update_application_fields";
    case ccs::gui_ipc::GuiCommand::SetLightweightMode:
        return "set_lightweight_mode";
    case ccs::gui_ipc::GuiCommand::StorageStatus: return "storage_status";
    case ccs::gui_ipc::GuiCommand::MigrateStorage: return "migrate_storage";
    case ccs::gui_ipc::GuiCommand::AddRule:
    case ccs::gui_ipc::GuiCommand::RemoveRule:
    case ccs::gui_ipc::GuiCommand::SetRuleEnabled:
    case ccs::gui_ipc::GuiCommand::MoveRule:
    case ccs::gui_ipc::GuiCommand::UpdateRuleOptions:
    case ccs::gui_ipc::GuiCommand::PreviewRules:
        return "unsupported";
    }
    return "unknown";
}

class RecordingTray final {
public:
    explicit RecordingTray(std::size_t expected_commands)
        : expected_commands_(expected_commands) {
        const auto nonce = std::to_wstring(
            std::chrono::steady_clock::now().time_since_epoch().count());
        pipe_name_ = L"\\\\.\\pipe\\ccs-trans.feature-controller-test." + nonce;
    }

    ~RecordingTray() {
        if (thread_.joinable()) thread_.join();
    }

    std::string pipe_name() const {
        return std::string(pipe_name_.begin(), pipe_name_.end());
    }

    void start() {
        thread_ = std::thread([this] {
            try {
                run();
            } catch (const std::exception& exception) {
                error_ = exception.what();
            }
            finished_.store(true);
        });
    }

    void join() {
        if (thread_.joinable()) thread_.join();
        require(error_.empty(), error_);
    }

    std::vector<ccs::gui_ipc::Command> commands() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return commands_;
    }

private:
    void run() {
        const HANDLE pipe = CreateNamedPipeW(
            pipe_name_.c_str(),
            PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT
                | PIPE_REJECT_REMOTE_CLIENTS,
            1,
            64U * 1024U,
            64U * 1024U,
            0,
            nullptr);
        require(pipe != INVALID_HANDLE_VALUE, "failed to create fake tray pipe");
        const bool connected = ConnectNamedPipe(pipe, nullptr) != FALSE
            || GetLastError() == ERROR_PIPE_CONNECTED;
        require(connected, "failed to accept GUI client");

        std::string error;
        ccs::gui_ipc::Envelope envelope;
        require(ccs::gui_ipc::parse_envelope(read_frame(pipe), envelope, error), error);
        ccs::gui_ipc::Hello hello;
        require(envelope.kind == ccs::gui_ipc::MessageKind::Hello
                && ccs::gui_ipc::parse_hello(envelope.payload_json, hello, error),
            "GUI did not send a typed hello");

        ccs::gui_ipc::HelloResult hello_result{
            true,
            hello.version,
            hello.source_commit,
            std::string(kSessionId),
            1,
            ccs::gui_ipc::ErrorCode::None,
            {},
        };
        envelope = server_envelope(
            ccs::gui_ipc::MessageKind::HelloResult, 1, hello, "hello");
        envelope.result = ccs::gui_ipc::ResultCode::Accepted;
        envelope.error_code = ccs::gui_ipc::ErrorCode::None;
        require(ccs::gui_ipc::serialize_hello_result(
                    hello_result, envelope.payload_json, error), error);
        write_frame(pipe, envelope);

        auto snapshot = fixture_snapshot();
        envelope = server_envelope(
            ccs::gui_ipc::MessageKind::Snapshot, 1, hello, "snapshot");
        envelope.base_revision = snapshot.draft.base_revision;
        require(ccs::gui_ipc::serialize_snapshot(
                    snapshot, envelope.payload_json, error), error);
        write_frame(pipe, envelope);

        std::uint64_t next_server_sequence = 2;
        for (std::size_t index = 0; index < expected_commands_; ++index) {
            require(ccs::gui_ipc::parse_envelope(
                        read_frame(pipe), envelope, error), error);
            ccs::gui_ipc::Command command;
            require(envelope.kind == ccs::gui_ipc::MessageKind::Command
                    && ccs::gui_ipc::parse_command(
                        envelope.payload_json, command, error),
                "GUI did not send a typed command");
            {
                std::lock_guard<std::mutex> lock(mutex_);
                commands_.push_back(command);
            }

            ccs::gui_ipc::CommandStatus status;
            status.sequence = envelope.sequence;
            status.command = completion_command_name(command.command);
            status.outcome = ccs::gui_ipc::ResultCode::Succeeded;
            const auto request_id = envelope.request_id;

            if (command.command == ccs::gui_ipc::GuiCommand::SaveProfile
                && snapshot.profile_editor) {
                apply_field_edits(snapshot.profile_editor->fields, command.field_edits);
                for (const auto& field : snapshot.profile_editor->fields) {
                    if (!field.value) continue;
                    if (field.key == "id") {
                        if (const auto* value = std::get_if<std::string>(&*field.value)) {
                            snapshot.profile_editor->profile_id = *value;
                            snapshot.selection.profile_id = *value;
                            if (snapshot.rules_editor) {
                                snapshot.rules_editor->profile_id = *value;
                            }
                            for (auto& profile : snapshot.profiles) {
                                if (profile.key == snapshot.profile_editor->key) {
                                    profile.id = *value;
                                }
                            }
                        }
                    } else if (field.key == "enabled") {
                        if (const auto* value = std::get_if<bool>(&*field.value)) {
                            for (auto& profile : snapshot.profiles) {
                                if (profile.key == snapshot.profile_editor->key) {
                                    profile.enabled = *value;
                                }
                            }
                        }
                    }
                }
            } else if (command.command
                == ccs::gui_ipc::GuiCommand::UpdateApplicationFields) {
                apply_field_edits(snapshot.application_fields, command.field_edits);
            } else if (command.command
                == ccs::gui_ipc::GuiCommand::ReplaceRulesText) {
                if (snapshot.rules_editor) snapshot.rules_editor->text = command.text;
            } else if (command.command
                == ccs::gui_ipc::GuiCommand::MigrateStorage) {
                snapshot.storage.state = "ready";
                snapshot.storage.detail = "storage is ready";
            }
            ++snapshot.revision;
            ++snapshot.draft.revision;
            snapshot.last_command = status;

            const auto send_status = [&] {
                auto message = server_envelope(
                    ccs::gui_ipc::MessageKind::CommandStatus,
                    next_server_sequence++, hello, request_id);
                message.result = status.outcome;
                message.error_code = status.error;
                require(ccs::gui_ipc::serialize_command_status(
                            status, message.payload_json, error), error);
                write_frame(pipe, message);
            };
            const auto send_snapshot = [&] {
                auto message = server_envelope(
                    ccs::gui_ipc::MessageKind::Snapshot,
                    next_server_sequence++, hello,
                    "state-" + std::to_string(snapshot.revision));
                message.base_revision = snapshot.draft.base_revision;
                require(ccs::gui_ipc::serialize_snapshot(
                            snapshot, message.payload_json, error), error);
                write_frame(pipe, message);
            };
            const bool snapshot_first = command.command
                    == ccs::gui_ipc::GuiCommand::SaveProfile
                || command.command
                    == ccs::gui_ipc::GuiCommand::ReplaceRulesText;
            if (snapshot_first) {
                send_snapshot();
                send_status();
            } else {
                send_status();
                send_snapshot();
            }
        }
        FlushFileBuffers(pipe);
        std::array<char, 1> disconnect_probe{};
        DWORD received = 0;
        (void)ReadFile(
            pipe, disconnect_probe.data(), 1, &received, nullptr);
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
    }

    std::size_t expected_commands_;
    std::wstring pipe_name_;
    std::thread thread_;
    mutable std::mutex mutex_;
    std::vector<ccs::gui_ipc::Command> commands_;
    std::string error_;
    std::atomic_bool finished_{false};
};

class GuiFeatureControllerTests final : public QObject {
    Q_OBJECT

private slots:
    void profileSettingsRulesAndMigrationCommands() {
        RecordingTray tray(7);
        tray.start();
        ccs::gui_ipc::LaunchBootstrap bootstrap{
            tray.pipe_name(),
            "0.8-test",
            "feature-controller-source",
            "feature-controller-instance",
            "feature-controller-token",
            std::string(kSessionId),
        };
        ccs_trans::gui::GuiIpcClient client(std::move(bootstrap));
        ccs_trans::gui::GuiStateStore state(client);
        ccs_trans::gui::CommandDispatcher commands(client);
        ccs_trans::gui::ProfilesController profiles(state, commands);
        ccs_trans::gui::RulesController rules(state, commands);
        ccs_trans::gui::SettingsController settings(state, commands);
        ccs_trans::gui::MigrationController migration(state, commands);
        auto* application = qobject_cast<QGuiApplication*>(
            QCoreApplication::instance());
        QVERIFY(application != nullptr);
        ccs_trans::gui::GuiWindowController window_controller(
            *application, client, state, commands);
        QSignalSpy command_spy(
            &commands, &ccs_trans::gui::CommandDispatcher::commandFinished);
        QSignalSpy close_blocked_spy(
            &window_controller, &ccs_trans::gui::GuiWindowController::closeBlocked);

        client.start();
        QTRY_VERIFY_WITH_TIMEOUT(client.ready(), 3000);
        QTRY_COMPARE_WITH_TIMEOUT(state.revision(), 1ULL, 3000);
        QCOMPARE(profiles.profileKey(), QStringLiteral("2"));

        profiles.setProfileId(QStringLiteral("beta-renamed"));
        QVERIFY(profiles.dirty());
        profiles.save();
        QTRY_COMPARE_WITH_TIMEOUT(command_spy.count(), 1, 3000);
        QTRY_VERIFY_WITH_TIMEOUT(!profiles.dirty(), 3000);
        QCOMPARE(profiles.profileId(), QStringLiteral("beta-renamed"));

        profiles.moveSelected(0);
        QTRY_COMPARE_WITH_TIMEOUT(command_spy.count(), 2, 3000);

        QVERIFY(settings.setFieldValue(
            QStringLiteral("listener.port"), QVariant::fromValue<qulonglong>(17070)));
        QVERIFY(settings.dirty());
        settings.save();
        window_controller.requestClose(false);
        QCOMPARE(close_blocked_spy.count(), 1);
        QVERIFY(!window_controller.closePromptVisible());
        QTRY_COMPARE_WITH_TIMEOUT(command_spy.count(), 3, 3000);
        QTRY_VERIFY_WITH_TIMEOUT(!settings.dirty(), 3000);

        rules.setText(QStringLiteral("a\r\nb\rc\u2028d\u2029e"));
        QCOMPARE(rules.text(), QStringLiteral("a\nb\nc\nd\ne"));
        rules.save();
        QTRY_COMPARE_WITH_TIMEOUT(command_spy.count(), 4, 3000);
        QTRY_VERIFY_WITH_TIMEOUT(!rules.dirty(), 3000);

        QSignalSpy migration_confirmation_spy(
            &migration, &ccs_trans::gui::MigrationController::confirmationChanged);
        migration.requestMigration();
        QVERIFY(migration.migrationConfirmationRequired());
        QCOMPARE(migration_confirmation_spy.count(), 1);
        migration.requestMigration();
        QCOMPARE(migration_confirmation_spy.count(), 1);
        migration.confirmMigration();
        QVERIFY(!migration.migrationConfirmationRequired());
        QVERIFY(migration.replacementConfirmationRequired());
        QCOMPARE(migration_confirmation_spy.count(), 2);
        migration.requestMigration();
        QCOMPARE(migration_confirmation_spy.count(), 2);
        migration.cancelReplacement();
        QVERIFY(!migration.replacementConfirmationRequired());
        migration.requestMigration();
        QVERIFY(migration.migrationConfirmationRequired());
        migration.confirmMigration();
        QVERIFY(migration.replacementConfirmationRequired());
        migration.confirmReplacement();
        QTRY_COMPARE_WITH_TIMEOUT(command_spy.count(), 5, 3000);
        QTRY_COMPARE_WITH_TIMEOUT(migration.state(), QStringLiteral("ready"), 3000);
        QVERIFY(!migration.migrationConfirmationRequired());
        QVERIFY(!migration.replacementConfirmationRequired());

        window_controller.requestClose(true);
        QVERIFY(window_controller.closePromptVisible());
        window_controller.resolveClose(QStringLiteral("cancel"));
        QVERIFY(!window_controller.closePromptVisible());

        window_controller.requestClose(true);
        window_controller.resolveClose(QStringLiteral("apply"));
        QCOMPARE(close_blocked_spy.count(), 2);
        QVERIFY(window_controller.closePromptVisible());
        window_controller.resolveClose(QStringLiteral("discard"));
        QTRY_COMPARE_WITH_TIMEOUT(command_spy.count(), 6, 3000);
        QVERIFY(!window_controller.closePromptVisible());

        window_controller.requestClose(false);
        QVERIFY(window_controller.closePromptVisible());
        window_controller.resolveClose(QStringLiteral("apply"));
        QTRY_COMPARE_WITH_TIMEOUT(command_spy.count(), 7, 3000);
        QVERIFY(!window_controller.closePromptVisible());

        client.stop();
        tray.join();
        const auto recorded = tray.commands();
        QCOMPARE(recorded.size(), std::size_t{7});
        QCOMPARE(recorded[0].command, ccs::gui_ipc::GuiCommand::SaveProfile);
        QCOMPARE(recorded[0].profile_key, std::optional<std::int64_t>{2});
        QCOMPARE(recorded[0].profile_id, std::string("beta-renamed"));
        QCOMPARE(recorded[1].command, ccs::gui_ipc::GuiCommand::MoveProfile);
        QCOMPARE(recorded[1].profile_key, std::optional<std::int64_t>{2});
        QCOMPARE(recorded[1].position, std::size_t{1});
        QCOMPARE(recorded[2].command,
            ccs::gui_ipc::GuiCommand::UpdateApplicationFields);
        QCOMPARE(recorded[3].command,
            ccs::gui_ipc::GuiCommand::ReplaceRulesText);
        QCOMPARE(recorded[3].text, std::string("a\nb\nc\nd\ne"));
        QCOMPARE(recorded[4].command, ccs::gui_ipc::GuiCommand::MigrateStorage);
        QVERIFY(recorded[4].replace_existing_storage);
        QCOMPARE(recorded[4].replacement_confirmation, std::string("REPLACE"));
        QCOMPARE(recorded[5].command, ccs::gui_ipc::GuiCommand::DiscardDraft);
        QCOMPARE(recorded[6].command, ccs::gui_ipc::GuiCommand::ApplyDraft);
    }
};

} // namespace

QTEST_MAIN(GuiFeatureControllerTests)

#include "gui_feature_controller_tests.moc"
