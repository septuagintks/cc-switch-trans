#pragma once

#include "config/config_document.hpp"
#include "protocols/protocol_registry.hpp"
#include "runtime/runtime_snapshot.hpp"

#include <filesystem>
#include <optional>
#include <string>

namespace ccs {

struct RuntimeCompileOptions {
    std::optional<std::string> selected_profile;
};

class RuntimeCompiler {
public:
    explicit RuntimeCompiler(
        std::filesystem::path application_root,
        std::shared_ptr<const ProtocolRegistry> protocols = builtin_protocol_registry());

    bool compile(
        const ConfigDocument& document,
        const RuntimeCompileOptions& options,
        RuntimeSnapshotPtr& snapshot,
        std::string& error) const;

private:
    std::filesystem::path application_root_;
    std::shared_ptr<const ProtocolRegistry> protocols_;
};

} // namespace ccs
