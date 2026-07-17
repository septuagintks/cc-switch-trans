#include "logging/logger.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <system_error>
#include <utility>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace ccs {

namespace {

class FileLogSink final : public LogSink {
public:
    FileLogSink(
        std::uint64_t max_total_size,
        std::shared_ptr<RuntimeMetrics> metrics)
        : max_total_size_(max_total_size)
        , metrics_(std::move(metrics)) {}

    bool open(const std::filesystem::path& path, std::string& error) override {
        path_ = path;
        if (max_total_size_ == 0) {
            error = "log max total size must be greater than zero";
            return false;
        }
        segment_target_size_ = std::min<std::uint64_t>(
            128ULL * 1024 * 1024,
            std::max<std::uint64_t>(1, max_total_size_ / 16));
        if (!acquire_lock(error)) {
            return false;
        }
        if (!scan_existing(error) || !compact_oversized_active(error)
            || !prune_archives(error)) {
            release_lock();
            return false;
        }
        file_.open(path, std::ios::app | std::ios::binary);
        if (!file_) {
            error = "failed to open log file: " + path.string();
            release_lock();
            return false;
        }
        storage_reporting_enabled_ = true;
        update_storage_metric();
        return true;
    }

    bool write(std::string_view data, std::string& error) override {
        if (data.size() > max_total_size_) {
            error = "log record exceeds the configured total size limit";
            return false;
        }
        if (active_size_ != 0
            && active_size_ + static_cast<std::uint64_t>(data.size()) > segment_target_size_
            && !rotate(error)) {
            return false;
        }
        file_.write(data.data(), static_cast<std::streamsize>(data.size()));
        if (!file_) {
            error = "failed to write log file: " + path_.string();
            return false;
        }
        active_size_ += static_cast<std::uint64_t>(data.size());
        managed_size_ += static_cast<std::uint64_t>(data.size());
        if (managed_size_ > max_total_size_ && !flush(error)) {
            return false;
        }
        if (!prune_archives(error)) {
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
        if (metrics_ && storage_reporting_enabled_) {
            metrics_->log_storage_changed(reported_storage_size_, 0);
        }
        reported_storage_size_ = 0;
        storage_reporting_enabled_ = false;
        release_lock();
    }

private:
    struct Archive {
        std::filesystem::path path;
        std::uint64_t sequence = 0;
        std::uint64_t size = 0;
    };

    static bool replace_file(
        const std::filesystem::path& source,
        const std::filesystem::path& target,
        std::string& error) {
#ifdef _WIN32
        if (!MoveFileExW(
                source.c_str(),
                target.c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            error = "failed to replace oversized log file: Windows error "
                + std::to_string(GetLastError());
            return false;
        }
#else
        if (std::rename(source.c_str(), target.c_str()) != 0) {
            error = "failed to replace oversized log file: "
                + std::string(std::strerror(errno));
            return false;
        }
#endif
        return true;
    }

    bool acquire_lock(std::string& error) {
        lock_path_ = path_;
        lock_path_ += ".ccs-lock";
#ifdef _WIN32
        lock_handle_ = CreateFileW(
            lock_path_.c_str(),
            GENERIC_READ | GENERIC_WRITE,
            0,
            nullptr,
            OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        if (lock_handle_ == INVALID_HANDLE_VALUE) {
            const auto code = GetLastError();
            error = code == ERROR_SHARING_VIOLATION || code == ERROR_LOCK_VIOLATION
                ? "log file is already owned by another process"
                : "failed to acquire log file lock: Windows error " + std::to_string(code);
            return false;
        }
#else
        lock_fd_ = ::open(lock_path_.c_str(), O_CREAT | O_RDWR, 0600);
        if (lock_fd_ < 0) {
            error = "failed to open log file lock: " + std::string(std::strerror(errno));
            return false;
        }
        if (flock(lock_fd_, LOCK_EX | LOCK_NB) != 0) {
            error = errno == EWOULDBLOCK
                ? "log file is already owned by another process"
                : "failed to acquire log file lock: " + std::string(std::strerror(errno));
            ::close(lock_fd_);
            lock_fd_ = -1;
            return false;
        }
#endif
        return true;
    }

    void release_lock() noexcept {
#ifdef _WIN32
        if (lock_handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(lock_handle_);
            lock_handle_ = INVALID_HANDLE_VALUE;
        }
#else
        if (lock_fd_ >= 0) {
            flock(lock_fd_, LOCK_UN);
            ::close(lock_fd_);
            lock_fd_ = -1;
        }
#endif
    }

    std::filesystem::path archive_path(std::uint64_t sequence) const {
        std::ostringstream suffix;
        suffix << ".ccs-archive-" << std::setw(20) << std::setfill('0') << sequence;
        auto result = path_;
        result += suffix.str();
        return result;
    }

    bool parse_archive_sequence(
        const std::filesystem::path& candidate,
        std::uint64_t& sequence) const {
        auto prefix_path = path_.filename();
        prefix_path += ".ccs-archive-";
        const auto& prefix = prefix_path.native();
        const auto filename_path = candidate.filename();
        const auto& filename = filename_path.native();
        if (filename.size() <= prefix.size()
            || !std::equal(prefix.begin(), prefix.end(), filename.begin())) {
            return false;
        }
        sequence = 0;
        constexpr auto zero = static_cast<std::filesystem::path::value_type>('0');
        constexpr auto nine = static_cast<std::filesystem::path::value_type>('9');
        for (auto index = prefix.size(); index < filename.size(); ++index) {
            const auto ch = filename[index];
            if (ch < zero || ch > nine) {
                return false;
            }
            const auto digit = static_cast<std::uint64_t>(ch - zero);
            if (sequence > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) {
                return false;
            }
            sequence = sequence * 10 + digit;
        }
        return sequence != 0;
    }

    bool has_managed_prefix(
        const std::filesystem::path& candidate,
        const char* marker) const {
        auto prefix_path = path_.filename();
        prefix_path += marker;
        const auto& prefix = prefix_path.native();
        const auto filename_path = candidate.filename();
        const auto& filename = filename_path.native();
        return filename.size() > prefix.size()
            && std::equal(prefix.begin(), prefix.end(), filename.begin());
    }

    bool scan_existing(std::string& error) {
        archives_.clear();
        active_size_ = 0;
        managed_size_ = 0;
        next_archive_sequence_ = 1;
        std::error_code ec;
        if (std::filesystem::exists(path_, ec)) {
            active_size_ = std::filesystem::file_size(path_, ec);
            if (ec) {
                error = "failed to inspect log file size: " + ec.message();
                return false;
            }
            managed_size_ = active_size_;
        } else if (ec) {
            error = "failed to inspect log file: " + ec.message();
            return false;
        }

        auto parent = path_.parent_path();
        if (parent.empty()) {
            parent = std::filesystem::current_path(ec);
            if (ec) {
                error = "failed to resolve log directory: " + ec.message();
                return false;
            }
        }
        for (std::filesystem::directory_iterator iterator(parent, ec), end;
             !ec && iterator != end;
             iterator.increment(ec)) {
            if (has_managed_prefix(iterator->path(), ".ccs-compact-")) {
                const auto status = iterator->symlink_status(ec);
                if (ec) {
                    break;
                }
                if (!std::filesystem::is_regular_file(status)) {
                    continue;
                }
                const auto size = iterator->file_size(ec);
                if (ec) {
                    break;
                }
                const bool removed = std::filesystem::remove(iterator->path(), ec);
                if (ec) {
                    error = "failed to remove stale log compaction file: " + ec.message();
                    return false;
                }
                if (!removed) {
                    error = "stale log compaction file disappeared before cleanup";
                    return false;
                }
                if (metrics_) {
                    metrics_->log_retention_removed(1, size);
                }
                continue;
            }
            std::uint64_t sequence = 0;
            if (!parse_archive_sequence(iterator->path(), sequence)) {
                continue;
            }
            const auto status = iterator->symlink_status(ec);
            if (ec) {
                break;
            }
            if (!std::filesystem::is_regular_file(status)) {
                continue;
            }
            const auto size = iterator->file_size(ec);
            if (ec) {
                break;
            }
            archives_.push_back(Archive{iterator->path(), sequence, size});
            managed_size_ += size;
            if (sequence == std::numeric_limits<std::uint64_t>::max()) {
                error = "log archive sequence is exhausted";
                return false;
            }
            next_archive_sequence_ = std::max(next_archive_sequence_, sequence + 1);
        }
        if (ec) {
            error = "failed to scan log archives: " + ec.message();
            return false;
        }
        std::sort(archives_.begin(), archives_.end(), [](const Archive& left, const Archive& right) {
            return left.sequence < right.sequence;
        });
        return true;
    }

    bool compact_oversized_active(std::string& error) {
        if (active_size_ <= max_total_size_) {
            return true;
        }
        std::ifstream input(path_, std::ios::binary);
        if (!input) {
            error = "failed to open oversized log file for compaction: " + path_.string();
            return false;
        }
        const auto original_size = active_size_;
        std::uint64_t start = original_size - max_total_size_;
        if (start != 0) {
            input.seekg(static_cast<std::streamoff>(start - 1));
            char previous = '\0';
            input.read(&previous, 1);
            if (!input) {
                error = "failed to inspect oversized log record boundary";
                return false;
            }
            if (previous != '\n') {
                input.clear();
                input.seekg(static_cast<std::streamoff>(start));
                std::array<char, 8192> scan{};
                bool boundary_found = false;
                std::uint64_t cursor = start;
                while (input && !boundary_found) {
                    input.read(scan.data(), static_cast<std::streamsize>(scan.size()));
                    const auto count = input.gcount();
                    if (count <= 0) {
                        break;
                    }
                    const auto* newline = static_cast<const char*>(
                        std::memchr(scan.data(), '\n', static_cast<std::size_t>(count)));
                    if (newline) {
                        start = cursor + static_cast<std::uint64_t>(newline - scan.data()) + 1;
                        boundary_found = true;
                    }
                    cursor += static_cast<std::uint64_t>(count);
                }
                if (!boundary_found) {
                    start = original_size;
                }
            }
        }

        auto temporary = path_;
        temporary += ".ccs-compact-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count());
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) {
            error = "failed to create compacted log file: " + temporary.string();
            return false;
        }
        input.clear();
        input.seekg(static_cast<std::streamoff>(start));
        std::array<char, 64 * 1024> buffer{};
        while (input) {
            input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            const auto count = input.gcount();
            if (count > 0) {
                output.write(buffer.data(), count);
            }
        }
        if (!input.eof() || !output) {
            error = "failed while compacting oversized log file";
            output.close();
            std::error_code cleanup_error;
            std::filesystem::remove(temporary, cleanup_error);
            return false;
        }
        output.flush();
        output.close();
        input.close();
        if (!output) {
            error = "failed to flush compacted log file";
            std::error_code cleanup_error;
            std::filesystem::remove(temporary, cleanup_error);
            return false;
        }
        if (!replace_file(temporary, path_, error)) {
            std::error_code cleanup_error;
            std::filesystem::remove(temporary, cleanup_error);
            return false;
        }
        active_size_ = original_size - start;
        managed_size_ -= original_size - active_size_;
        if (metrics_) {
            metrics_->log_retention_removed(0, original_size - active_size_);
        }
        update_storage_metric();
        return true;
    }

    bool rotate(std::string& error) {
        if (next_archive_sequence_ == std::numeric_limits<std::uint64_t>::max()) {
            error = "log archive sequence is exhausted";
            return false;
        }
        if (!flush(error)) {
            return false;
        }
        file_.close();
        if (file_.fail()) {
            error = "failed to close log file before rotation: " + path_.string();
            return false;
        }
        const auto destination = archive_path(next_archive_sequence_);
        std::error_code ec;
        std::filesystem::rename(path_, destination, ec);
        if (ec) {
            error = "failed to rotate log file: " + ec.message();
            return false;
        }
        archives_.push_back(Archive{destination, next_archive_sequence_, active_size_});
        ++next_archive_sequence_;
        active_size_ = 0;
        file_.clear();
        file_.open(path_, std::ios::app | std::ios::binary);
        if (!file_) {
            error = "failed to open new log file after rotation: " + path_.string();
            return false;
        }
        if (metrics_) {
            metrics_->log_rotated();
        }
        return true;
    }

    bool prune_archives(std::string& error) {
        while (managed_size_ > max_total_size_ && !archives_.empty()) {
            const auto oldest = archives_.front();
            std::error_code ec;
            if (!std::filesystem::remove(oldest.path, ec) || ec) {
                error = "failed to remove expired log archive: "
                    + (ec ? ec.message() : oldest.path.string());
                return false;
            }
            archives_.erase(archives_.begin());
            managed_size_ -= oldest.size;
            if (metrics_) {
                metrics_->log_retention_removed(1, oldest.size);
            }
        }
        if (managed_size_ > max_total_size_) {
            error = "active log file exceeds the configured total size limit";
            return false;
        }
        update_storage_metric();
        return true;
    }

    void update_storage_metric() {
        if (metrics_ && storage_reporting_enabled_) {
            metrics_->log_storage_changed(reported_storage_size_, managed_size_);
            reported_storage_size_ = managed_size_;
        }
    }

    std::filesystem::path path_;
    std::filesystem::path lock_path_;
    std::ofstream file_;
    std::uint64_t max_total_size_ = 0;
    std::uint64_t segment_target_size_ = 0;
    std::uint64_t active_size_ = 0;
    std::uint64_t managed_size_ = 0;
    std::uint64_t next_archive_sequence_ = 1;
    std::vector<Archive> archives_;
    std::shared_ptr<RuntimeMetrics> metrics_;
    std::uint64_t reported_storage_size_ = 0;
    bool storage_reporting_enabled_ = false;
#ifdef _WIN32
    HANDLE lock_handle_ = INVALID_HANDLE_VALUE;
#else
    int lock_fd_ = -1;
#endif
};

std::size_t escaped_size(std::string_view value) {
    std::size_t size = 0;
    for (const unsigned char ch : value) {
        switch (ch) {
        case '\\':
        case '"':
        case '\b':
        case '\f':
        case '\n':
        case '\r':
        case '\t':
            size += 2;
            break;
        default:
            size += ch < 0x20 ? 6 : 1;
            break;
        }
    }
    return size;
}

void append_escaped(std::string& output, std::string_view value) {
    constexpr char hex[] = "0123456789abcdef";
    for (const unsigned char ch : value) {
        switch (ch) {
        case '\\': output += "\\\\"; break;
        case '"': output += "\\\""; break;
        case '\b': output += "\\b"; break;
        case '\f': output += "\\f"; break;
        case '\n': output += "\\n"; break;
        case '\r': output += "\\r"; break;
        case '\t': output += "\\t"; break;
        default:
            if (ch < 0x20) {
                output += "\\u00";
                output.push_back(hex[(ch >> 4) & 0x0f]);
                output.push_back(hex[ch & 0x0f]);
            } else {
                output.push_back(static_cast<char>(ch));
            }
            break;
        }
    }
}

std::array<char, 24> timestamp() {
    const auto now = std::chrono::system_clock::now();
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &time);
#else
    localtime_r(&time, &local);
#endif

    std::array<char, 24> output{};
    if (std::strftime(output.data(), 20, "%Y-%m-%dT%H:%M:%S", &local) == 0) {
        constexpr std::string_view fallback = "1970-01-01T00:00:00.000";
        std::copy(fallback.begin(), fallback.end(), output.begin());
        return output;
    }
    std::snprintf(
        output.data() + 19,
        output.size() - 19,
        ".%03lld",
        static_cast<long long>(millis.count()));
    return output;
}

int level_value(std::string_view level) {
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

bool ascii_case_equal(std::string_view left, std::string_view right) {
    return left.size() == right.size()
        && std::equal(left.begin(), left.end(), right.begin(), [](char lhs, char rhs) {
            return std::tolower(static_cast<unsigned char>(lhs))
                == std::tolower(static_cast<unsigned char>(rhs));
        });
}

bool is_sensitive_header(std::string_view name) {
    static constexpr std::array<std::string_view, 6> sensitive = {
        "authorization",
        "proxy-authorization",
        "cookie",
        "set-cookie",
        "x-api-key",
        "api-key",
    };
    return std::any_of(sensitive.begin(), sensitive.end(), [&](std::string_view candidate) {
        return ascii_case_equal(name, candidate);
    });
}

std::string_view field_text(const LogField& field) {
    if (const auto* owned = std::get_if<std::string>(&field.value)) {
        return *owned;
    }
    if (const auto* borrowed = std::get_if<std::string_view>(&field.value)) {
        return *borrowed;
    }
    return {};
}

std::size_t headers_size(const LogHeadersRef& reference) {
    std::size_t size = 2;
    bool first = true;
    if (!reference.headers) {
        return size;
    }
    for (const auto& [name, value] : *reference.headers) {
        const std::string_view rendered = reference.redact_sensitive && is_sensitive_header(name)
            ? std::string_view("***")
            : std::string_view(value);
        size += (first ? 0 : 1) + 5 + escaped_size(name) + escaped_size(rendered);
        first = false;
    }
    return size;
}

void append_headers(std::string& output, const LogHeadersRef& reference) {
    output.push_back('{');
    bool first = true;
    if (reference.headers) {
        for (const auto& [name, value] : *reference.headers) {
            if (!first) {
                output.push_back(',');
            }
            first = false;
            output.push_back('"');
            append_escaped(output, name);
            output += "\":\"";
            append_escaped(
                output,
                reference.redact_sensitive && is_sensitive_header(name)
                    ? std::string_view("***")
                    : std::string_view(value));
            output.push_back('"');
        }
    }
    output.push_back('}');
}

std::size_t rendered_line_size(
    std::string_view level,
    std::string_view event,
    const std::vector<LogField>& fields) {
    constexpr std::size_t timestamp_size = 23;
    std::size_t size = 32 + timestamp_size + escaped_size(level) + escaped_size(event);
    for (const auto& field : fields) {
        size += 4 + escaped_size(field.name);
        if (const auto* headers = std::get_if<LogHeadersRef>(&field.value)) {
            size += headers_size(*headers);
        } else {
            const auto value = field_text(field);
            size += field.quoted ? escaped_size(value) + 2 : value.size();
        }
    }
    return size + 2;
}

std::string render_line(
    std::string_view level,
    std::string_view event,
    const std::vector<LogField>& fields) {
    std::string output;
    output.reserve(rendered_line_size(level, event, fields));
    output += "{\"time\":\"";
    const auto rendered_timestamp = timestamp();
    append_escaped(output, std::string_view(rendered_timestamp.data(), 23));
    output += "\",\"level\":\"";
    append_escaped(output, level);
    output += "\",\"event\":\"";
    append_escaped(output, event);
    output.push_back('"');
    for (const auto& field : fields) {
        output += ",\"";
        append_escaped(output, field.name);
        output += "\":";
        if (const auto* headers = std::get_if<LogHeadersRef>(&field.value)) {
            append_headers(output, *headers);
            continue;
        }
        const auto value = field_text(field);
        if (field.quoted) {
            output.push_back('"');
            append_escaped(output, value);
            output.push_back('"');
        } else {
            output.append(value);
        }
    }
    output += "}\n";
    return output;
}

} // namespace

LogField field_string(std::string name, std::string value) {
    return LogField{std::move(name), std::move(value), true};
}

LogField field_string_view(std::string name, std::string_view value) {
    return LogField{std::move(name), value, true};
}

LogField field_number(std::string name, long long value) {
    return LogField{std::move(name), std::to_string(value), false};
}

LogField field_bool(std::string name, bool value) {
    return LogField{std::move(name), std::string(value ? "true" : "false"), false};
}

LogField field_raw(std::string name, std::string raw_json) {
    return LogField{std::move(name), std::move(raw_json), false};
}

LogField field_headers(
    std::string name,
    const Headers& headers,
    bool redact_sensitive) {
    return LogField{
        std::move(name),
        LogHeadersRef{&headers, redact_sensitive},
        false,
    };
}

Logger::Logger(
    LoggerConfig config,
    std::shared_ptr<RuntimeMetrics> metrics,
    std::unique_ptr<LogSink> sink,
    FailureHandler failure_handler,
    std::shared_ptr<InflightMemoryBudget> inflight_budget)
    : config_(std::move(config))
    , metrics_(std::move(metrics))
    , inflight_budget_(std::move(inflight_budget))
    , sink_(std::move(sink))
    , failure_handler_(std::move(failure_handler)) {
    if (!sink_) {
        sink_ = std::make_unique<FileLogSink>(config_.max_total_size, metrics_);
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
            metrics_->log_writer_failed(false);
        }
        return false;
    };
    const auto parent = config_.path.parent_path();
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
        sink_opened = sink_->open(config_.path, error);
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
        metrics_writer_active_ = true;
        metrics_->log_writer_started();
    }
    return true;
}

bool Logger::log(std::string level, std::string event, std::initializer_list<LogField> fields) const {
    return log(std::move(level), std::move(event), std::vector<LogField>(fields));
}

bool Logger::enabled(std::string_view level) const noexcept {
    return level_value(level) >= level_value(config_.level);
}

bool Logger::log(std::string level, std::string event, const std::vector<LogField>& fields) const {
    if (!enabled(level)) {
        return true;
    }

    const bool immediate_flush = level == "error";
    const auto reserved_line_size = rendered_line_size(level, event, fields);
    std::optional<InflightMemoryBudget::Lease> memory;
    if (inflight_budget_) {
        memory = inflight_budget_->try_acquire(reserved_line_size);
        if (!memory) {
            report_record_rejection("log record rejected by the inflight memory budget");
            return false;
        }
    }
    std::string line;
    try {
        line = render_line(level, event, fields);
    } catch (...) {
        report_record_rejection("log record rendering failed");
        return false;
    }
    const auto line_size = line.size();
    if (memory && line_size < memory->bytes()) {
        memory->shrink(memory->bytes() - line_size);
    } else if (memory && line_size > memory->bytes()
        && !memory->try_grow(line_size - memory->bytes())) {
        report_record_rejection("rendered log record exceeded its inflight memory lease");
        return false;
    }

    std::unique_lock<std::mutex> lock(mutex_);
    if (state_ != LogWriterState::Running || stopping_) {
        return false;
    }
    const auto has_space = [&]() {
        return stopping_ || state_ != LogWriterState::Running || pending_records_ == 0
            || pending_bytes_ + line_size <= config_.queue_capacity;
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

    const auto sequence = next_sequence_;
    const auto enqueued_at = std::chrono::steady_clock::now();
    try {
        queue_.push_back(QueuedRecord{
            sequence,
            std::move(memory),
            std::move(line),
            immediate_flush,
            enqueued_at,
        });
    } catch (...) {
        lock.unlock();
        report_record_rejection("log queue allocation failed");
        return false;
    }
    ++next_sequence_;
    if (pending_records_ == 0) {
        oldest_pending_at_ = enqueued_at;
    }
    ++pending_records_;
    pending_bytes_ += line_size;
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

bool Logger::drain(std::string& error) const {
    error.clear();
    std::unique_lock<std::mutex> lock(mutex_);
    if (state_ != LogWriterState::Running || stopping_) {
        error = writer_error_.empty() ? "log writer is not running" : writer_error_;
        return false;
    }
    const auto target_sequence = next_sequence_ - 1;
    if (target_sequence == 0 || flushed_sequence_ >= target_sequence) {
        return true;
    }
    if (!queue_.empty()) {
        queue_.back().immediate_flush = true;
    }
    queue_cv_.notify_one();
    flushed_cv_.wait(lock, [&]() {
        return flushed_sequence_ >= target_sequence
            || state_ != LogWriterState::Running
            || stopping_;
    });
    if (flushed_sequence_ >= target_sequence) {
        return true;
    }
    error = writer_error_.empty() ? "log writer stopped before draining" : writer_error_;
    return false;
}

LogWriterStatus Logger::status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return LogWriterStatus{state_, writer_error_, pending_records_, pending_bytes_};
}

void Logger::writer_loop() {
    const auto flush_interval = std::chrono::milliseconds(config_.flush_interval_ms);
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

        const auto batch_size = batch.size();
        const auto last_sequence = batch.back().sequence;
        batch.clear();

        if (!failure.empty()) {
            report_writer_failure(std::move(failure));
            return;
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            pending_records_ -= batch_size;
            pending_bytes_ -= batch_bytes;
            flushed_sequence_ = last_sequence;
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
                    batch_size,
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

    bool was_active = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ == LogWriterState::Running) {
            state_ = LogWriterState::Stopped;
        }
        was_active = metrics_writer_active_;
        metrics_writer_active_ = false;
    }
    if (metrics_ && was_active) {
        metrics_->log_writer_stopped();
    }
    flushed_cv_.notify_all();
    space_cv_.notify_all();
}

void Logger::report_record_rejection(std::string_view error) const noexcept {
    try {
        if (failure_handler_) {
            failure_handler_(std::string(error));
            return;
        }
    } catch (...) {
    }
    std::fputs("ccs-trans logger rejected a record: ", stderr);
    std::fwrite(error.data(), 1, error.size(), stderr);
    std::fputc('\n', stderr);
}

void Logger::report_writer_failure(std::string error) {
    bool was_active = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.clear();
        pending_records_ = 0;
        pending_bytes_ = 0;
        oldest_pending_at_ = {};
        state_ = LogWriterState::Failed;
        writer_error_ = error;
        was_active = metrics_writer_active_;
        metrics_writer_active_ = false;
    }
    if (metrics_) {
        metrics_->log_queue_reset();
        metrics_->log_writer_failed(was_active);
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
