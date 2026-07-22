#pragma once

#include "gui_ipc/protocol_types.hpp"

#include <string>
#include <string_view>

namespace ccs::gui_ipc {

bool serialize_envelope(
    const Envelope& envelope,
    std::string& content,
    std::string& error);
bool parse_envelope(
    std::string_view content,
    Envelope& envelope,
    std::string& error);

bool serialize_hello(const Hello& value, std::string& content, std::string& error);
bool parse_hello(std::string_view content, Hello& value, std::string& error);
bool serialize_hello_result(
    const HelloResult& value,
    std::string& content,
    std::string& error);
bool parse_hello_result(
    std::string_view content,
    HelloResult& value,
    std::string& error);
bool serialize_launch_bootstrap(
    const LaunchBootstrap& value,
    std::string& content,
    std::string& error);
bool parse_launch_bootstrap(
    std::string_view content,
    LaunchBootstrap& value,
    std::string& error);
bool serialize_command(
    const Command& value,
    std::string& content,
    std::string& error);
bool parse_command(std::string_view content, Command& value, std::string& error);
bool serialize_snapshot(
    const Snapshot& value,
    std::string& content,
    std::string& error);
bool parse_snapshot(std::string_view content, Snapshot& value, std::string& error);
bool serialize_state_delta(
    const StateDelta& value,
    std::string& content,
    std::string& error);
bool parse_state_delta(
    std::string_view content,
    StateDelta& value,
    std::string& error);
bool serialize_command_status(
    const CommandStatus& value,
    std::string& content,
    std::string& error);
bool parse_command_status(
    std::string_view content,
    CommandStatus& value,
    std::string& error);
bool serialize_maintenance_request(
    const MaintenanceRequest& value,
    std::string& content,
    std::string& error);
bool parse_maintenance_request(
    std::string_view content,
    MaintenanceRequest& value,
    std::string& error);
bool serialize_maintenance_result(
    const MaintenanceResult& value,
    std::string& content,
    std::string& error);
bool parse_maintenance_result(
    std::string_view content,
    MaintenanceResult& value,
    std::string& error);

} // namespace ccs::gui_ipc
