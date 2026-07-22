#pragma once

#include "config/config_repository.hpp"

#include <cstddef>
#include <istream>
#include <optional>
#include <ostream>
#include <string>

namespace ccs {

class CompositeConfigRepository;

enum class ConfigCliCommandKind {
    Run,
    ConfigShow,
    ConfigSet,
    ConfigUnset,
    ProfileList,
    ProfileShow,
    ProfileCreate,
    ProfileRemove,
    ProfileEnable,
    ProfileDisable,
    ProfileSet,
    ProfileUnset,
    ProfileRename,
    ProfileMove,
    RuleList,
    RuleShow,
    RuleAdd,
    RuleRemove,
    RuleEnable,
    RuleDisable,
    RuleSet,
    RuleUnset,
    RuleMove,
    StorageStatus,
    StorageMigrate,
    StorageVerify,
    Help,
    Version,
};

struct ConfigCliCommand {
    ConfigCliCommandKind kind = ConfigCliCommandKind::Run;
    std::string profile_id;
    std::string rule_id;
    std::string rule_type;
    std::string key;
    std::string value;
    std::size_t position = 0;
    std::string run_profile;
    std::string run_log_level;
    std::string run_log_path;
    bool storage_replace = false;
    std::optional<std::string> storage_confirmation_token;
};

struct ConfigCliParseResult {
    bool ok = false;
    std::string error;
    ConfigCliCommand command;
};

ConfigCliParseResult parse_config_cli(int argc, char** argv);
bool is_config_cli_management_command(const std::string& command);
bool confirm_storage_replacement(
    const ConfigCliCommand& command,
    bool input_is_terminal,
    std::istream& input,
    std::ostream& prompt,
    std::string& error);
bool execute_config_cli(
    const ConfigCliCommand& command,
    ConfigRepository& repository,
    std::string& output,
    std::string& error);
bool execute_config_cli(
    const ConfigCliCommand& command,
    CompositeConfigRepository& repository,
    std::string& output,
    std::string& error);
void print_config_cli_help(std::ostream& output);
void print_config_cli_version(std::ostream& output);

} // namespace ccs
