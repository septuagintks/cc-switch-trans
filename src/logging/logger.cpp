#include "logging/logger.hpp"

#include <chrono>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <utility>

namespace ccs {

namespace {

std::string json_escape(const std::string& value) {
    std::ostringstream out;
    for (const unsigned char ch : value) {
        switch (ch) {
        case '\\':
            out << "\\\\";
            break;
        case '"':
            out << "\\\"";
            break;
        case '\b':
            out << "\\b";
            break;
        case '\f':
            out << "\\f";
            break;
        case '\n':
            out << "\\n";
            break;
        case '\r':
            out << "\\r";
            break;
        case '\t':
            out << "\\t";
            break;
        default:
            if (ch < 0x20) {
                out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(ch);
            } else {
                out << static_cast<char>(ch);
            }
            break;
        }
    }
    return out.str();
}

std::string timestamp() {
    const auto now = std::chrono::system_clock::now();
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &time);
#else
    localtime_r(&time, &local);
#endif

    std::ostringstream out;
    out << std::put_time(&local, "%Y-%m-%dT%H:%M:%S")
        << "." << std::setw(3) << std::setfill('0') << millis.count();
    return out.str();
}

int level_value(const std::string& level) {
    if (level == "trace") {
        return 0;
    }
    if (level == "debug") {
        return 1;
    }
    if (level == "info") {
        return 2;
    }
    if (level == "warn") {
        return 3;
    }
    if (level == "error") {
        return 4;
    }
    return 2;
}

} // namespace

LogField field_string(std::string name, std::string value) {
    return LogField{std::move(name), std::move(value), true};
}

LogField field_number(std::string name, long long value) {
    return LogField{std::move(name), std::to_string(value), false};
}

LogField field_bool(std::string name, bool value) {
    return LogField{std::move(name), value ? "true" : "false", false};
}

LogField field_raw(std::string name, std::string raw_json) {
    return LogField{std::move(name), std::move(raw_json), false};
}

Logger::Logger(AppConfig config)
    : config_(std::move(config)) {}

bool Logger::open(std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);

    const auto parent = config_.log_path.parent_path();
    if (!parent.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            error = "failed to create log directory: " + ec.message();
            return false;
        }
    }

    file_.open(config_.log_path, std::ios::app);
    if (!file_) {
        error = "failed to open log file: " + config_.log_path.string();
        return false;
    }
    return true;
}

void Logger::log(std::string level, std::string event, std::initializer_list<LogField> fields) const {
    log(std::move(level), std::move(event), std::vector<LogField>(fields));
}

void Logger::log(std::string level, std::string event, const std::vector<LogField>& fields) const {
    if (level_value(level) < level_value(config_.log_level)) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (!file_) {
        return;
    }

    file_ << "{\"time\":\"" << json_escape(timestamp()) << "\""
          << ",\"level\":\"" << json_escape(level) << "\""
          << ",\"event\":\"" << json_escape(event) << "\"";

    for (const auto& field : fields) {
        file_ << ",\"" << json_escape(field.name) << "\":";
        if (field.quoted) {
            file_ << "\"" << json_escape(field.value) << "\"";
        } else {
            file_ << field.value;
        }
    }

    file_ << "}\n";
    file_.flush();
}

} // namespace ccs
