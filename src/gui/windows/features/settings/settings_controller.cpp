#include "features/settings/settings_controller.hpp"

#include "controllers/command_dispatcher.hpp"
#include "state/gui_state_store.hpp"

#include <utility>

namespace ccs_trans::gui {

SettingsController::SettingsController(
    GuiStateStore& state,
    CommandDispatcher& commands,
    QObject* parent)
    : QObject(parent), state_(state), commands_(commands) {
    connect(&state_, &GuiStateStore::preferenceChanged,
            this, &SettingsController::lightweightModeChanged);
}

bool SettingsController::lightweightMode() const noexcept {
    return state_.lightweightMode();
}

void SettingsController::setLightweightMode(bool enabled) {
    if (enabled == state_.lightweightMode()) return;
    ccs::gui_ipc::Command request;
    request.command = ccs::gui_ipc::GuiCommand::SetLightweightMode;
    request.enabled = enabled;
    (void)commands_.submit(std::move(request));
}

} // namespace ccs_trans::gui
