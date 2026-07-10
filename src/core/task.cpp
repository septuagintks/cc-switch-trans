#include "core/task.hpp"

namespace ccs {

const char* task_name(ApiTaskKind kind) {
    switch (kind) {
    case ApiTaskKind::Responses:
        return "responses";
    case ApiTaskKind::ChatCompletions:
        return "chat_completions";
    case ApiTaskKind::Usage:
        return "usage";
    }
    return "unknown";
}

} // namespace ccs
