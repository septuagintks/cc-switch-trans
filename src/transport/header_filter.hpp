#pragma once

#include "core/http_types.hpp"

namespace ccs {

Headers filter_request_headers(const Headers& headers);
Headers filter_response_headers(const Headers& headers);

} // namespace ccs
