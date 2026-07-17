#pragma once

#include "core/inflight_memory_budget.hpp"

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace ccs {

using Headers = std::vector<std::pair<std::string, std::string>>;

struct HttpRequest {
    std::string request_id;
    std::string method;
    std::string target;
    std::string path;
    std::string query;
    std::string version;
    Headers headers;
    std::shared_ptr<InflightMemoryBudget::Lease> body_memory;
    std::string body;
    std::string client_ip;
};

struct HttpResponse {
    int status_code = 200;
    std::string reason = "OK";
    Headers headers;
    std::shared_ptr<InflightMemoryBudget::Lease> body_memory;
    std::string body;
};

struct UpstreamTarget {
    std::string base_url;
    std::string path;
};

} // namespace ccs
