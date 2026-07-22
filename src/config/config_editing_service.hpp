#pragma once

#include "config/config_repository.hpp"

#include <string>

namespace ccs {

enum class ConfigValidationError {
    None,
    InvalidDocument,
    UnknownProtocol,
    InvalidProfile,
    InvalidRules,
    InvalidLogPath,
    RouteCollision,
    RuntimeCompileFailed,
};

struct ConfigValidationFailure {
    ConfigValidationError code = ConfigValidationError::None;
    std::string profile_id;
    std::string field;
    std::string detail;
};

bool validate_config_candidate(
    const ConfigDocument& document,
    const std::filesystem::path& application_root,
    std::string& error);
bool validate_config_candidate(
    const ConfigDocument& document,
    const std::filesystem::path& application_root,
    ConfigValidationFailure& failure);

class ConfigEditingService {
public:
    explicit ConfigEditingService(ConfigRepository& repository);

    bool begin(std::string& error);
    bool validate(std::string& error) const;
    bool commit(std::string& error);
    void discard() noexcept;

    bool active() const noexcept;
    ConfigDocument& draft();
    const ConfigDocument& draft() const;

private:
    ConfigRepository& repository_;
    ConfigDocument draft_;
    bool active_ = false;
};

} // namespace ccs
