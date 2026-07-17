#pragma once

#include "config/application_config.hpp"
#include "config/config_repository.hpp"
#include "config/configuration_snapshot.hpp"

#include <string>

namespace ccs {

enum class StorageState {
    Uninitialized,
    MigrationRequired,
    Ready,
    RecoveryRequired,
    Invalid,
};

struct StorageStatus {
    StorageState state = StorageState::Uninitialized;
    ProfileRevision profile_revision = 0;
    std::optional<std::string> migrated_from_sha256;
    std::string detail;
};

enum class MigrationOutcome {
    Migrated,
    AlreadyMigrated,
};

class CompositeConfigRepository final : public ConfigRepository {
public:
    explicit CompositeConfigRepository(AppPaths paths);

    bool load(std::string& error) override;
    bool save(const ConfigDocument& document, std::string& error) override;

    bool save_snapshot(
        const ConfigurationSnapshot& desired,
        ConfigurationSnapshot& committed,
        std::string& error);
    bool inspect_storage(StorageStatus& status, std::string& error);
    bool migrate_v2(MigrationOutcome& outcome, std::string& error);
    bool verify_storage(std::string& error);

    bool loaded() const override;
    const ConfigDocument& document() const override;
    const ConfigurationSnapshot& snapshot() const;
    const AppPaths& paths() const override;
    ConfigRepositoryFailure last_failure() const noexcept override;

private:
    bool load_ready_locked(std::string& error);
    bool initialize_fresh_locked(std::string& error);
    bool recover_locked(std::string& error);
    bool update_legacy_document(std::string& error);

    AppPaths paths_;
    ConfigurationSnapshot snapshot_;
    ConfigDocument legacy_document_;
    bool loaded_ = false;
    ConfigRepositoryFailure last_failure_ = ConfigRepositoryFailure::None;
};

const char* storage_state_name(StorageState state) noexcept;

} // namespace ccs
