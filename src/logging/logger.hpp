#pragma once

#include "core/http_types.hpp"
#include "core/inflight_memory_budget.hpp"
#include "core/runtime_metrics.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <variant>
#include <vector>

namespace ccs {

struct LogHeadersRef {
    const Headers* headers = nullptr;
    bool redact_sensitive = true;
};

struct LogField {
    std::string name;
    std::variant<std::string, std::string_view, LogHeadersRef> value;
    bool quoted = true;
};

LogField field_string(std::string name, std::string value);
LogField field_string_view(std::string name, std::string_view value);
LogField field_number(std::string name, long long value);
LogField field_bool(std::string name, bool value);
LogField field_raw(std::string name, std::string raw_json);
LogField field_headers(
    std::string name,
    const Headers& headers,
    bool redact_sensitive);

class LogSink {
public:
    virtual ~LogSink() = default;

    virtual bool open(const std::filesystem::path& path, std::string& error) = 0;
    virtual bool write(std::string_view data, std::string& error) = 0;
    virtual bool flush(std::string& error) = 0;
    virtual void close() noexcept = 0;
};

enum class LogWriterState {
    Closed,
    Running,
    Failed,
    Stopped,
};

struct LogWriterStatus {
    LogWriterState state = LogWriterState::Closed;
    std::string error;
    std::size_t pending_records = 0;
    std::size_t pending_bytes = 0;
};

struct LoggerConfig {
    std::filesystem::path path;
    std::string level = "info";
    std::size_t queue_capacity = 16 * 1024 * 1024;
    std::uint64_t max_total_size = 2ULL * 1024 * 1024 * 1024;
    int flush_interval_ms = 100;
};

class Logger {
public:
    using FailureHandler = std::function<void(const std::string&)>;

    explicit Logger(
        LoggerConfig config,
        std::shared_ptr<RuntimeMetrics> metrics = {},
        std::unique_ptr<LogSink> sink = {},
        FailureHandler failure_handler = {},
        std::shared_ptr<InflightMemoryBudget> inflight_budget = {});
    ~Logger();

    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    bool open(std::string& error);
    bool enabled(std::string_view level) const noexcept;
    bool log(std::string level, std::string event, std::initializer_list<LogField> fields) const;
    bool log(std::string level, std::string event, const std::vector<LogField>& fields) const;
    bool drain(std::string& error) const;
    LogWriterStatus status() const;

private:
    struct QueuedRecord {
        std::uint64_t sequence = 0;
        std::optional<InflightMemoryBudget::Lease> memory;
        std::string line;
        bool immediate_flush = false;
        std::chrono::steady_clock::time_point enqueued_at;
    };

    void writer_loop();
    void report_writer_failure(std::string error);

    LoggerConfig config_;
    std::shared_ptr<RuntimeMetrics> metrics_;
    std::shared_ptr<InflightMemoryBudget> inflight_budget_;
    std::unique_ptr<LogSink> sink_;
    FailureHandler failure_handler_;
    mutable std::mutex mutex_;
    mutable std::condition_variable queue_cv_;
    mutable std::condition_variable space_cv_;
    mutable std::condition_variable flushed_cv_;
    mutable std::deque<QueuedRecord> queue_;
    mutable std::size_t pending_records_ = 0;
    mutable std::size_t pending_bytes_ = 0;
    mutable std::chrono::steady_clock::time_point oldest_pending_at_{};
    mutable std::uint64_t next_sequence_ = 1;
    mutable std::uint64_t flushed_sequence_ = 0;
    mutable bool stopping_ = false;
    mutable LogWriterState state_ = LogWriterState::Closed;
    mutable std::string writer_error_;
    bool metrics_writer_active_ = false;
    std::thread writer_;
};

} // namespace ccs
