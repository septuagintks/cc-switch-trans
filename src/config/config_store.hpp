#pragma once

#include "config/config_repository.hpp"

#include <string>

namespace ccs {

class ConfigStore final : public ConfigRepository {
public:
    explicit ConfigStore(AppPaths paths);

    bool load(std::string& error) override;
    bool save(const ConfigDocument& document, std::string& error) override;

    bool loaded() const override;
    const ConfigDocument& document() const override;
    const AppPaths& paths() const override;

private:
    bool source_is_unchanged(std::string& error) const;

    AppPaths paths_;
    ConfigDocument document_;
    std::string source_content_;
    bool source_exists_ = false;
    bool loaded_ = false;
};

} // namespace ccs
