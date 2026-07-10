#include "logging/logger.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <utility>

namespace ccs {

namespace {

class FileLogSink final : public LogSink {
public:
    bool open(const std::filesystem::path& path, std::string& error) override {
        path_ = path;
        file_.open(path, std::ios::app | std::ios::binary);
        if (!file_) {
            error = "failed to open log file: " + path.string();
            return false;
        }
        return true;
    }

    bool write(std::string_view data, std::string& error) override {
        file_.write(data.data(), static_cast<std::streamsize>(data.size()));
        if (!file_) {
            error = "failed to write log file: " + path_.string();
            return false;
        }
        return true;
    }

    bool flush(std::string& error) override {
        file_.flush();
        if (!file_) {
            error = "failed to flush log file: " + path_.string();
            return false;
        }
        return true;
    }

    void close() noexcept override {
        file_.close();
    }

private:
    std::filesystem::path path_;
    std::ofstream file_;
};

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

Logger::Logger(
    AppConfig config,
    std::shared_ptr<RuntimeMetrics> metrics,
    std::unique_ptr<LogSink> sink,
    FailureHandler failure_handler)
    : config_(std::move(config))
    , metrics_(std::move(metrics))
    , sink_(std::move(sink))
    , failure_handler_(std::move(failure_handler)) {
    if (!sink_) {
        sink_ = std::make_unique<FileLogSink>();
    }
}

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
    sink_->close();
}

bool Logger::open(std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ != LogWriterState::Closed) {
        error = "log writer has already been opened";
        return false;
    }
    const auto fail_open = [&]() {
        state_ = LogWriterState::Failed;
        writer_error_ = error;
        if (metrics_) {
            metrics_->log_writer_failed();
        }
        return false;
    };
    const auto parent = config_.log_path.parent_path();
    if (!parent.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            error = "failed to create log directory: " + ec.message();
            return fail_open();
        }
    }

    bool sink_opened = false;
    try {
        sink_opened = sink_->open(config_.log_path, error);
    } catch (const std::exception& ex) {
        error = "log sink open failed: " + std::string(ex.what());
    } catch (...) {
        error = "log sink open failed with an unknown exception";
    }
    if (!sink_opened) {
        if (error.empty()) {
            error = "log sink failed to open without an error message";
        }
        return fail_open();
    }

    try {
        writer_ = std::thread([this]() { writer_loop(); });
    } catch (const std::exception& ex) {
        error = "failed to start log writer: " + std::string(ex.what());
        sink_->close();
        return fail_open();
    }
    state_ = LogWriterState::Running;
    if (metrics_) {
        metrics_->log_writer_started();
    }
    return true;
}

bool Logger::log(std::string level, std::string event, std::initializer_list<LogField> fields) const {
    return log(std::move(level), std::move(event), std::vector<LogField>(fields));
}

bool Logger::log(std::string level, std::string event, const std::vector<LogField>& fields) const {
    if (level_value(level) < level_value(config_.log_level)) {
        return true;
    }

    const bool immediate_flush = level == "error";
    auto line = render_line(level, event, fields);
    const auto line_size = line.size();

    std::unique_lock<std::mutex> lock(mutex_);
    if (state_ != LogWriterState::Running || stopping_) {
        return false;
    }
    const auto has_space = [&]() {
        return stopping_ || state_ != LogWriterState::Running || pending_records_ == 0
            || pending_bytes_ + line_size <= config_.log_queue_capacity;
    };
    const bool backpressured = !has_space();
    const auto wait_started = std::chrono::steady_clock::now();
    space_cv_.wait(lock, has_space);
    if (backpressured && metrics_) {
        const auto waited = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - wait_started);
        metrics_->log_backpressure(static_cast<std::uint64_t>(waited.count()));
    }
    if (stopping_ || state_ != LogWriterState::Running) {
        return false;
    }

    const auto sequence = next_sequence_++;
    const auto enqueued_at = std::chrono::steady_clock::now();
    if (pending_records_ == 0) {
        oldest_pending_at_ = enqueued_at;
    }
    ++pending_records_;
    pending_bytes_ += line_size;
    queue_.push_back(QueuedRecord{sequence, std::move(line), immediate_flush, enqueued_at});
    if (metrics_) {
        const auto oldest = oldest_pending_at_.time_since_epoch();
        metrics_->log_record_enqueued(
            pending_records_,
            pending_bytes_,
            static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(oldest).count()));
    }
    queue_cv_.notify_one();

    if (immediate_flush) {
        flushed_cv_.wait(lock, [&]() {
            return flushed_sequence_ >= sequence || state_ != LogWriterState::Running || stopping_;
        });
        return flushed_sequence_ >= sequence;
    }
    return true;
}

LogWriterStatus Logger::status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return LogWriterStatus{state_, writer_error_, pending_records_, pending_bytes_};
}

void Logger::writer_loop() {
    const auto flush_interval = std::chrono::milliseconds(config_.log_flush_interval_ms);
    while (true) {
        std::deque<QueuedRecord> batch;
        std::size_t batch_bytes = 0;
        std::chrono::microseconds batch_window_wait{0};
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
                const auto flush_deadline = queue_.front().enqueued_at + flush_interval;
                const auto wait_started = std::chrono::steady_clock::now();
                queue_cv_.wait_until(lock, flush_deadline, [&]() {
                    return stopping_ || has_immediate();
                });
                batch_window_wait = std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - wait_started);
            }

            batch.swap(queue_);
            for (const auto& record : batch) {
                batch_bytes += record.line.size();
            }
        }

        const auto write_started = std::chrono::steady_clock::now();
        std::string failure;
        try {
            for (const auto& record : batch) {
                if (!sink_->write(record.line, failure)) {
                    break;
                }
            }
        } catch (const std::exception& ex) {
            failure = "log sink write failed: " + std::string(ex.what());
        } catch (...) {
            failure = "log sink write failed with an unknown exception";
        }
        const auto write_elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - write_started);
        const auto flush_started = std::chrono::steady_clock::now();
        if (failure.empty()) {
            try {
                if (!sink_->flush(failure) && failure.empty()) {
                    failure = "log sink flush failed without an error message";
                }
            } catch (const std::exception& ex) {
                failure = "log sink flush failed: " + std::string(ex.what());
            } catch (...) {
                failure = "log sink flush failed with an unknown exception";
            }
        }
        const auto flush_elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - flush_started);
        const auto oldest_age = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - batch.front().enqueued_at);

        if (!failure.empty()) {
            report_writer_failure(std::move(failure));
            return;
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            pending_records_ -= batch.size();
            pending_bytes_ -= batch_bytes;
            flushed_sequence_ = batch.back().sequence;
            if (!queue_.empty()) {
                oldest_pending_at_ = queue_.front().enqueued_at;
            } else {
                oldest_pending_at_ = {};
            }
            if (metrics_) {
                std::uint64_t oldest_pending_ns = 0;
                if (pending_records_ != 0) {
                    oldest_pending_ns = static_cast<std::uint64_t>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(
                            oldest_pending_at_.time_since_epoch()).count());
                }
                metrics_->log_batch_written(
                    batch.size(),
                    batch_bytes,
                    static_cast<std::uint64_t>(batch_window_wait.count()),
                    static_cast<std::uint64_t>(write_elapsed.count()),
                    static_cast<std::uint64_t>(flush_elapsed.count()),
                    static_cast<std::uint64_t>(oldest_age.count()),
                    pending_records_,
                    pending_bytes_,
                    oldest_pending_ns);
            }
        }
        flushed_cv_.notify_all();
        space_cv_.notify_all();
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ == LogWriterState::Running) {
            state_ = LogWriterState::Stopped;
        }
    }
    if (metrics_) {
        metrics_->log_writer_stopped();
    }
    flushed_cv_.notify_all();
    space_cv_.notify_all();
}

void Logger::report_writer_failure(std::string error) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_ = LogWriterState::Failed;
        writer_error_ = error;
    }
    if (metrics_) {
        metrics_->log_writer_failed();
    }
    try {
        if (failure_handler_) {
            failure_handler_(error);
        } else {
            std::cerr << "log writer failed: " << error << "\n";
        }
    } catch (const std::exception& ex) {
        std::cerr << "log writer failure handler failed: " << ex.what() << "\n";
    } catch (...) {
        std::cerr << "log writer failure handler failed with an unknown exception\n";
    }
    flushed_cv_.notify_all();
    space_cv_.notify_all();
}

} // namespace ccs
