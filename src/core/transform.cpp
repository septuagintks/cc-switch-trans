#include "core/transform.hpp"

#include <utility>

namespace ccs {

TransformError::TransformError(int status_code, std::string response_type, std::string message)
    : std::runtime_error(std::move(message))
    , status_code_(status_code)
    , response_type_(std::move(response_type)) {}

int TransformError::status_code() const {
    return status_code_;
}

const std::string& TransformError::response_type() const {
    return response_type_;
}

} // namespace ccs
