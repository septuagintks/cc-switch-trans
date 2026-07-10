#pragma once

#include "core/task.hpp"

#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace ccs {

struct RemovedTool {
    std::string type;
    std::string name;
};

struct TransformResult {
    bool matched = false;
    bool modified = false;
    std::string rewrite_name;
    std::string rewrite_reason;
    std::size_t original_body_size = 0;
    std::size_t rewritten_body_size = 0;
    std::optional<std::string> rewritten_body;
    std::vector<RemovedTool> removed_tools;
};

class TransformError : public std::runtime_error {
public:
    TransformError(int status_code, std::string response_type, std::string message);

    int status_code() const;
    const std::string& response_type() const;

private:
    int status_code_;
    std::string response_type_;
};

class RequestTransform {
public:
    virtual ~RequestTransform() = default;
    virtual TransformResult apply(const TaskConfig& task, const std::string& body) const = 0;
};

} // namespace ccs
