#pragma once

#ifdef _WIN32

#include "gui_ipc/protocol_types.hpp"
#include "presentation/main_window_contract.hpp"

namespace ccs {

gui_ipc::Snapshot build_gui_snapshot(const MainWindowState& state);
gui_ipc::StateDelta build_gui_state_delta(
    const gui_ipc::Snapshot& previous,
    const gui_ipc::Snapshot& current);
gui_ipc::CommandStatus build_gui_command_status(
    const CommandResult& result,
    std::uint64_t client_sequence);

gui_ipc::ErrorCode gui_error_code(MainWindowError error) noexcept;
gui_ipc::ResultCode gui_result_code(CommandOutcome outcome) noexcept;

} // namespace ccs

#endif
