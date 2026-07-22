#pragma once

#include "gui_ipc/protocol_types.hpp"

#include <QStringList>

#include <cstdint>
#include <string>

namespace ccs_trans::gui {

bool bootstrap_handle_from_arguments(
    const QStringList& arguments,
    std::uintptr_t& handle,
    QString& error);

bool read_launch_bootstrap(
    std::uintptr_t inherited_handle,
    ccs::gui_ipc::LaunchBootstrap& bootstrap,
    std::string& error);

} // namespace ccs_trans::gui
