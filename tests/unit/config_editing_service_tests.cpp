#include "config/app_paths.hpp"
#include "config/config_editing_service.hpp"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

class MemoryConfigRepository final : public ccs::ConfigRepository {
public:
    explicit MemoryConfigRepository(ccs::AppPaths paths)
        : paths_(std::move(paths)) {}

    bool load(std::string& error) override {
        error.clear();
        loaded_ = true;
        return true;
    }

    bool save(const ccs::ConfigDocument& document, std::string& error) override {
        if (fail_save_) {
            error = "injected repository save failure";
            return false;
        }
        document_ = document;
        ++save_count_;
        return true;
    }

    bool loaded() const override {
        return loaded_;
    }

    const ccs::ConfigDocument& document() const override {
        return document_;
    }

    const ccs::AppPaths& paths() const override {
        return paths_;
    }

    void fail_save(bool value) {
        fail_save_ = value;
    }

    std::size_t save_count() const {
        return save_count_;
    }

private:
    ccs::AppPaths paths_;
    ccs::ConfigDocument document_ = ccs::make_default_config_document();
    bool loaded_ = false;
    bool fail_save_ = false;
    std::size_t save_count_ = 0;
};

ccs::ProfileDefinition complete_profile(const std::string& prefix) {
    ccs::ProfileDefinition profile;
    profile.enabled = true;
    profile.protocol = ccs::ProtocolId{"responses"};
    profile.local.request_path = prefix + "/v1/responses";
    profile.upstream.base_url = "https://example.com";
    profile.upstream.request_path = "/v1/responses";
    return profile;
}

void test_draft_validation_and_commit_boundary() {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path()
        / ("ccs-trans-config-editor-" + std::to_string(nonce));
    MemoryConfigRepository repository(ccs::make_app_paths(root));
    ccs::ConfigEditingService editing(repository);
    std::string error;

    require(!editing.begin(error) && error.find("not loaded") != std::string::npos,
        "editing requires an explicitly loaded repository");
    require(repository.load(error), error);
    require(editing.begin(error) && editing.active(), error);
    editing.draft().application.listener.port = 16000;
    require(repository.document().application.listener.port == 15723,
        "draft changes do not mutate repository state");
    require(editing.validate(error), error);
    require(editing.commit(error) && !editing.active(), error);
    require(repository.document().application.listener.port == 16000
            && repository.save_count() == 1,
        "validated draft commits once through the repository interface");

    require(editing.begin(error), error);
    editing.draft().profiles.emplace("incomplete", ccs::ProfileDefinition{});
    editing.draft().profiles.at("incomplete").enabled = true;
    require(!editing.validate(error) && error.find("enabled but missing") != std::string::npos,
        "invalid enabled Profile is rejected before persistence");
    require(repository.save_count() == 1 && editing.active(),
        "failed validation leaves repository unchanged and draft available");
    editing.discard();
    require(!editing.active(), "discard retires the draft");
}

void test_runtime_semantics_and_repository_failure() {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path()
        / ("ccs-trans-config-editor-runtime-" + std::to_string(nonce));
    MemoryConfigRepository repository(ccs::make_app_paths(root));
    std::string error;
    require(repository.load(error), error);
    ccs::ConfigEditingService editing(repository);

    require(editing.begin(error), error);
    editing.draft().profiles.emplace("first", complete_profile("/shared"));
    editing.draft().profiles.emplace("second", complete_profile("/shared"));
    require(!editing.validate(error) && error.find("route collision") != std::string::npos,
        "draft validation compiles enabled Profiles and detects route collisions");
    editing.discard();

    require(editing.begin(error), error);
    editing.draft().profiles.emplace("first", complete_profile("/first"));
    repository.fail_save(true);
    require(!editing.commit(error)
            && error == "injected repository save failure"
            && editing.active(),
        "repository failure preserves the validated draft for retry");
    repository.fail_save(false);
    require(editing.commit(error) && repository.save_count() == 1, error);
}

} // namespace

int main() {
    try {
        test_draft_validation_and_commit_boundary();
        test_runtime_semantics_and_repository_failure();
        std::cout << "config editing service tests ok\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "config editing service tests failed: " << ex.what() << "\n";
        return 1;
    }
}
