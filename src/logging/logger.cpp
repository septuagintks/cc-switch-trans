#include "logging/logger.hpp"

#include <algorithm>
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

std::string render_line(const std::string& level, const std::string& event, const std::vector<LogField>& fields) {
    std::ostringstream out;
    out << "{\"time\":\"" << json_escape(timestamp()) << "\""
        << ",\"level\":\"" << json_escape(level) << "\""
        << ",\"event\":\"" << json_escape(event) << "\"";
    for (const auto& field : fields) {
        out << ",\"" << json_escape(field.name) << "\":";
        if (field.quoted) {
            out << "\"" << json_escape(field.value) << "\"";
        } else {
            out << field.value;
        }
    }
    out << "}\n";
    return out.str();
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

Logger::~Logger() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    queue_cv_.notify_all();
    space_cv_.notify_all();
    if (writer_.joinable()) {
        writer_.join();
    }
}

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

    file_.open(config_.log_path, std::ios::app | std::ios::binary);
    if (!file_) {
        error = "failed to open log file: " + config_.log_path.string();
        return false;
    }

    try {
        writer_ = std::thread([this]() { writer_loop(); });
    } catch (const std::exception& ex) {
        error = "failed to start log writer: " + std::string(ex.what());
        file_.close();
        return false;
    }
    opened_ = true;
    return true;
}

void Logger::log(std::string level, std::string event, std::initializer_list<LogField> fields) const {
    log(std::move(level), std::move(event), std::vector<LogField>(fields));
}

void Logger::log(std::string level, std::string event, const std::vector<LogField>& fields) const {
    if (level_value(level) < level_value(config_.log_level)) {
        return;
    }

    const bool immediate_flush = level == "error";
    auto line = render_line(level, event, fields);
    const auto line_size = line.size();

    std::unique_lock<std::mutex> lock(mutex_);
    if (!opened_ || stopping_ || writer_failed_) {
        return;
    }
    space_cv_.wait(lock, [&]() {
        return stopping_ || writer_failed_ || queue_.empty()
            || queued_bytes_ + line_size <= config_.log_queue_capacity;
    });
    if (stopping_ || writer_failed_) {
        return;
    }

    const auto sequence = next_sequence_++;
    queued_bytes_ += line_size;
    queue_.push_back(QueuedRecord{sequence, std::move(line), immediate_flush});
    queue_cv_.notify_one();

    if (immediate_flush) {
        flushed_cv_.wait(lock, [&]() {
            return flushed_sequence_ >= sequence || writer_failed_ || stopping_;
        });
    }
}

void Logger::writer_loop() {
    const auto flush_interval = std::chrono::milliseconds(config_.log_flush_interval_ms);
    while (true) {
        std::deque<QueuedRecord> batch;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            queue_cv_.wait(lock, [&]() { return stopping_ || !queue_.empty(); });
            if (queue_.empty() && stopping_) {
                break;
            }

            const auto has_immediate = [&]() {
                return std::any_of(queue_.begin(), queue_.end(), [](const QueuedRecord& record) {
                    return record.immediate_flush;
                });
            };
            if (!stopping_ && !has_immediate()) {
                queue_cv_.wait_for(lock, flush_interval, [&]() {
                    return stopping_ || has_immediate();
                });
            }

            batch.swap(queue_);
            queued_bytes_ = 0;
            space_cv_.notify_all();
        }

        for (const auto& record : batch) {
            file_ << record.line;
        }
        file_.flush();

        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!file_) {
                writer_failed_ = true;
            }
            if (!batch.empty()) {
                flushed_sequence_ = batch.back().sequence;
            }
        }
        flushed_cv_.notify_all();
        if (writer_failed_) {
            space_cv_.notify_all();
            return;
        }
    }

    file_.flush();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!queue_.empty()) {
            writer_failed_ = true;
        }
    }
    flushed_cv_.notify_all();
}

} // namespace ccs
