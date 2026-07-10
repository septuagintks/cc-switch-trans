#pragma once

#include "config/config.hpp"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <fstream>
#include <initializer_list>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ccs {

struct LogField {
    std::string name;
    std::string value;
    bool quoted = true;
};

LogField field_string(std::string name, std::string value);
LogField field_number(std::string name, long long value);
LogField field_bool(std::string name, bool value);
LogField field_raw(std::string name, std::string raw_json);

class Logger {
public:
    explicit Logger(AppConfig config);
    ~Logger();

    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    bool open(std::string& error);
    void log(std::string level, std::string event, std::initializer_list<LogField> fields) const;
    void log(std::string level, std::string event, const std::vector<LogField>& fields) const;

private:
    struct QueuedRecord {
        std::uint64_t sequence = 0;
        std::string line;
        bool immediate_flush = false;
    };

    void writer_loop();

    AppConfig config_;
    mutable std::mutex mutex_;
    mutable std::condition_variable queue_cv_;
    mutable std::condition_variable space_cv_;
    mutable std::condition_variable flushed_cv_;
    mutable std::deque<QueuedRecord> queue_;
    mutable std::size_t queued_bytes_ = 0;
    mutable std::uint64_t next_sequence_ = 1;
    mutable std::uint64_t flushed_sequence_ = 0;
    mutable bool opened_ = false;
    mutable bool stopping_ = false;
    mutable bool writer_failed_ = false;
    std::ofstream file_;
    std::thread writer_;
};

} // namespace ccs
