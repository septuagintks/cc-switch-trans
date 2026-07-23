#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace ccs::gui_ipc {

inline constexpr std::string_view kProtocol = "ccs-trans.gui-ipc/v1";
inline constexpr std::string_view kMaintenanceProtocol =
    "ccs-trans.maintenance-ipc/v1";
inline constexpr std::size_t kMaximumFrameBytes = 16U * 1024U * 1024U;
inline constexpr std::size_t kDefaultOutboundQueueCapacity = 128;

enum class MessageKind {
    Hello,
    HelloResult,
    Activate,
    Shutdown,
    Ping,
    Pong,
    SnapshotRequest,
    Snapshot,
    StateChanged,
    Command,
    CommandStatus,
    MaintenanceRequest,
    MaintenanceResult,
};

enum class ResultCode {
    Accepted,
    Succeeded,
    Rejected,
    Failed,
    Cancelled,
    SavedPendingRuntimeApply,
};

enum class ErrorCode {
    None,
    MalformedFrame,
    MalformedMessage,
    UnsupportedProtocol,
    VersionMismatch,
    SourceMismatch,
    AuthenticationFailed,
    SessionMismatch,
    SequenceRejected,
    RevisionStale,
    Busy,
    InvalidArgument,
    ProfileNotFound,
    ProfileAlreadyExists,
    ValidationFailed,
    RouteCollision,
    RepositoryStale,
    DraftStale,
    PersistenceFailed,
    RuntimeApplyFailed,
    ServiceUnavailable,
    UnsavedChangesDecisionRequired,
    MigrationRequired,
    ReplacementConfirmationRequired,
    IpcBackpressure,
    Disconnected,
    Internal,
};

enum class GuiCommand {
    Refresh,
    StartService,
    StopService,
    ReloadService,
    QuitApplication,
    ApplyDraft,
    DiscardDraft,
    ReloadDraft,
    SelectProfile,
    CreateProfile,
    RemoveProfile,
    SaveProfile,
    SetProfileEnabled,
    MoveProfile,
    ReplaceRulesText,
    FormatRulesText,
    UpdateApplicationFields,
    SetLightweightMode,
    StorageStatus,
    MigrateStorage,
    AddRule,
    RemoveRule,
    SetRuleEnabled,
    MoveRule,
    UpdateRuleOptions,
    PreviewRules,
};

enum class UnsavedDecision {
    Apply,
    Discard,
    Cancel,
};

enum class MaintenanceCommand {
    QueryVersion,
    RequestShutdown,
    WaitForRelease,
};

struct Envelope {
    std::string protocol{std::string(kProtocol)};
    MessageKind kind = MessageKind::Ping;
    std::string request_id;
    std::string session_id;
    std::uint64_t sequence = 0;
    std::string base_revision;
    std::string source_commit;
    std::optional<ResultCode> result;
    std::optional<ErrorCode> error_code;
    std::string payload_json{"{}"};

    bool operator==(const Envelope&) const = default;
};

struct Hello {
    std::string version;
    std::string source_commit;
    std::string instance_identity;
    std::string handshake_token;
    std::uint64_t process_id = 0;

    bool operator==(const Hello&) const = default;
};

struct HelloResult {
    bool accepted = false;
    std::string version;
    std::string source_commit;
    std::string session_id;
    std::uint64_t state_revision = 0;
    ErrorCode error = ErrorCode::None;
    std::string detail;

    bool operator==(const HelloResult&) const = default;
};

struct LaunchBootstrap {
    std::string pipe_name_utf8;
    std::string version;
    std::string source_commit;
    std::string instance_identity;
    std::string handshake_token;
    std::string session_id;

    bool operator==(const LaunchBootstrap&) const = default;
};

using FieldValue = std::variant<std::string, std::uint64_t, bool>;

struct FieldEdit {
    std::string key;
    std::optional<FieldValue> value;

    bool operator==(const FieldEdit&) const = default;
};

struct Command {
    GuiCommand command = GuiCommand::Refresh;
    std::string profile_id;
    std::string replacement_profile_id;
    std::optional<std::int64_t> profile_key;
    bool enabled = false;
    std::size_t position = 0;
    std::vector<FieldEdit> field_edits;
    std::string text;
    std::optional<std::uint64_t> expected_draft_revision;
    std::optional<std::string> expected_base_revision;
    std::optional<UnsavedDecision> unsaved_decision;
    bool replace_existing_storage = false;
    std::string replacement_confirmation;

    bool operator==(const Command&) const = default;
};

struct ApplicationStatus {
    std::string state;
    std::string listener_host;
    std::uint16_t listener_port = 0;
    std::string last_error;
    int last_exit_code = 0;

    bool operator==(const ApplicationStatus&) const = default;
};

struct StorageStatus {
    std::string state;
    bool database_exists = false;
    std::string detail;
    std::string database_path;
    std::string backup_directory;

    bool operator==(const StorageStatus&) const = default;
};

struct FieldState {
    std::string key;
    std::string scope;
    std::string input_kind;
    bool required = true;
    std::optional<std::uint64_t> minimum;
    std::optional<std::uint64_t> maximum;
    std::vector<std::string> enum_values;
    std::string display_name_key;
    std::string apply_impact;
    std::optional<FieldValue> value;

    bool operator==(const FieldState&) const = default;
};

struct ProfileSummary {
    std::int64_t key = 0;
    std::string id;
    bool enabled = false;
    std::optional<std::string> protocol;
    std::string readiness;
    std::string status_detail;
    std::size_t rule_count = 0;
    std::size_t enabled_rule_count = 0;

    bool operator==(const ProfileSummary&) const = default;
};

struct ProfileEditor {
    std::int64_t key = 0;
    std::string profile_id;
    std::vector<FieldState> fields;

    bool operator==(const ProfileEditor&) const = default;
};

struct RulesDiagnostic {
    std::string message;
    std::size_t line = 1;
    std::size_t column = 1;
    std::string rule_id;
    std::string rule_type;
    std::string option;

    bool operator==(const RulesDiagnostic&) const = default;
};

struct RulesEditor {
    std::int64_t profile_key = 0;
    std::string profile_id;
    std::string text;
    std::optional<RulesDiagnostic> diagnostic;

    bool operator==(const RulesEditor&) const = default;
};

struct DraftStatus {
    std::string phase;
    bool runtime_apply_pending = false;
    std::uint64_t revision = 0;
    std::string base_revision;

    bool operator==(const DraftStatus&) const = default;
};

struct CommandStatus {
    std::uint64_t sequence = 0;
    std::string command;
    ResultCode outcome = ResultCode::Succeeded;
    ErrorCode error = ErrorCode::None;
    std::string profile_id;
    std::string field;
    std::string detail;
    std::optional<std::string> recovery_command;

    bool operator==(const CommandStatus&) const = default;
};

struct Selection {
    std::optional<std::string> profile_id;
    std::optional<std::int64_t> profile_key;

    bool operator==(const Selection&) const = default;
};

struct Snapshot {
    std::uint64_t revision = 0;
    ApplicationStatus application;
    std::vector<ProfileSummary> profiles;
    std::vector<FieldState> application_fields;
    Selection selection;
    std::optional<ProfileEditor> profile_editor;
    std::optional<RulesEditor> rules_editor;
    DraftStatus draft;
    std::optional<CommandStatus> last_command;
    StorageStatus storage;
    bool lightweight_mode = true;
    bool command_pending = false;

    bool operator==(const Snapshot&) const = default;
};

struct StateDelta {
    std::uint64_t from_revision = 0;
    std::uint64_t revision = 0;
    std::optional<ApplicationStatus> application;
    std::optional<std::vector<ProfileSummary>> profiles;
    std::optional<std::vector<FieldState>> application_fields;
    std::optional<Selection> selection;
    bool profile_editor_changed = false;
    std::optional<ProfileEditor> profile_editor;
    bool rules_editor_changed = false;
    std::optional<RulesEditor> rules_editor;
    std::optional<DraftStatus> draft;
    bool last_command_changed = false;
    std::optional<CommandStatus> last_command;
    std::optional<StorageStatus> storage;
    std::optional<bool> lightweight_mode;
    std::optional<bool> command_pending;

    [[nodiscard]] bool empty() const noexcept;
    bool operator==(const StateDelta&) const = default;
};

struct MaintenanceRequest {
    MaintenanceCommand command = MaintenanceCommand::QueryVersion;
    std::uint32_t timeout_ms = 0;

    bool operator==(const MaintenanceRequest&) const = default;
};

struct MaintenanceResult {
    bool succeeded = false;
    std::string version;
    std::string source_commit;
    std::string state;
    std::string detail;

    bool operator==(const MaintenanceResult&) const = default;
};

const char* message_kind_name(MessageKind value) noexcept;
const char* result_code_name(ResultCode value) noexcept;
const char* error_code_name(ErrorCode value) noexcept;
const char* gui_command_name(GuiCommand value) noexcept;
const char* unsaved_decision_name(UnsavedDecision value) noexcept;
const char* maintenance_command_name(MaintenanceCommand value) noexcept;

bool parse_message_kind(std::string_view value, MessageKind& parsed) noexcept;
bool parse_result_code(std::string_view value, ResultCode& parsed) noexcept;
bool parse_error_code(std::string_view value, ErrorCode& parsed) noexcept;
bool parse_gui_command(std::string_view value, GuiCommand& parsed) noexcept;
bool parse_unsaved_decision(std::string_view value, UnsavedDecision& parsed) noexcept;
bool parse_maintenance_command(
    std::string_view value,
    MaintenanceCommand& parsed) noexcept;

bool validate_envelope(const Envelope& envelope, std::string& error);

} // namespace ccs::gui_ipc
