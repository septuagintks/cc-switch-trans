#pragma once

#include <string>
#include <vector>

namespace ccs {

enum class ApiTaskKind {
    Responses,
    ChatCompletions,
    Usage,
};

struct UpstreamTarget {
    std::string base_url;
    std::string path;
};

struct TaskConfig {
    ApiTaskKind kind = ApiTaskKind::Responses;
    bool enabled = false;
    std::string method;
    std::string local_path;
    UpstreamTarget upstream;
    std::vector<std::string> transforms;
    bool log_request_chain = true;
};

const char* task_name(ApiTaskKind kind);

} // namespace ccs
