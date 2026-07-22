#pragma once

#ifdef _WIN32

#include "gui_ipc/protocol_types.hpp"
#include "presentation/main_window_view_model.hpp"

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>

namespace ccs {

bool translate_gui_command(
    const gui_ipc::Command& command,
    MainWindowCommandRequest& request,
    gui_ipc::ErrorCode& error_code,
    std::string& error);

struct GuiCommandCompletion {
    std::string request_id;
    std::uint64_t client_sequence = 0;
    gui_ipc::CommandStatus status;
};

struct GuiCommandSubmission {
    bool submitted = false;
    std::optional<GuiCommandCompletion> immediate;
};

class GuiCommandRouter {
public:
    explicit GuiCommandRouter(MainWindowViewModel& view_model);

    GuiCommandSubmission submit(
        const gui_ipc::Envelope& envelope,
        const gui_ipc::Command& command);
    std::optional<GuiCommandCompletion> observe(const MainWindowState& state);
    void disconnect() noexcept;

private:
    struct Pending {
        std::string request_id;
        std::uint64_t client_sequence = 0;
        std::uint64_t previous_model_sequence = 0;
    };

    static GuiCommandCompletion immediate_failure(
        const gui_ipc::Envelope& envelope,
        const gui_ipc::Command& command,
        gui_ipc::ErrorCode error,
        std::string detail);

    MainWindowViewModel& view_model_;
    std::mutex mutex_;
    std::optional<Pending> pending_;
};

} // namespace ccs

#endif
