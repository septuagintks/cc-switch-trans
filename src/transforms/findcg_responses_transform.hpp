#pragma once

#include "core/transform.hpp"

namespace ccs {

class FindcgResponsesTransform final : public RequestTransform {
public:
    TransformResult apply(const TaskConfig& task, const std::string& body) const override;
};

} // namespace ccs
