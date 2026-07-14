#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace ccs {

enum class UpstreamTimeoutPhase {
    Resolve,
    Connect,
    Send,
    ResponseHeader,
    StreamIdle,
    ResponseBody,
    Total,
};

struct RuntimeMetricsSnapshot {
    std::uint64_t connections_accepted = 0;
    std::uint64_t connections_rejected = 0;
    std::uint64_t connections_completed = 0;
    std::uint64_t current_connections = 0;
    std::uint64_t peak_connections = 0;
    std::uint64_t current_queued_connections = 0;
    std::uint64_t peak_queued_connections = 0;
    std::uint64_t current_active_workers = 0;
    std::uint64_t peak_active_workers = 0;
    std::uint64_t connection_queue_wait_time_us = 0;
    std::uint64_t max_connection_queue_wait_us = 0;
    std::uint64_t requests_started = 0;
    std::uint64_t requests_completed = 0;
    std::uint64_t requests_cancelled = 0;
    std::uint64_t client_disconnects = 0;
    std::uint64_t sse_streams_started = 0;
    std::uint64_t stream_chunks_forwarded = 0;
    std::uint64_t stream_bytes_forwarded = 0;
    std::uint64_t upstream_requests_started = 0;
    std::uint64_t upstream_requests_completed = 0;
    std::uint64_t upstream_requests_cancelled = 0;
    std::uint64_t upstream_requests_failed = 0;
    std::uint64_t upstream_resolve_timeouts = 0;
    std::uint64_t upstream_connect_timeouts = 0;
    std::uint64_t upstream_send_timeouts = 0;
    std::uint64_t upstream_response_header_timeouts = 0;
    std::uint64_t upstream_stream_idle_timeouts = 0;
    std::uint64_t upstream_response_body_timeouts = 0;
    std::uint64_t upstream_total_timeouts = 0;
    std::uint64_t upstream_connection_handles_created = 0;
    std::uint64_t upstream_request_handles_created = 0;
    std::uint64_t upstream_bytes_sent = 0;
    std::uint64_t upstream_bytes_received = 0;
    std::uint64_t winhttp_connecting_events = 0;
    std::uint64_t winhttp_connected_events = 0;
    std::uint64_t winhttp_connection_closed_events = 0;
    std::uint64_t log_records_enqueued = 0;
    std::uint64_t log_records_written = 0;
    std::uint64_t log_bytes_written = 0;
    std::uint64_t current_log_queue_records = 0;
    std::uint64_t peak_log_queue_records = 0;
    std::uint64_t current_log_queue_bytes = 0;
    std::uint64_t peak_log_queue_bytes = 0;
    std::uint64_t log_backpressure_count = 0;
    std::uint64_t log_backpressure_wait_us = 0;
    std::uint64_t log_batches_written = 0;
    std::uint64_t log_flush_count = 0;
    std::uint64_t log_write_time_us = 0;
    std::uint64_t log_batch_wait_time_us = 0;
    std::uint64_t max_log_batch_wait_us = 0;
    std::uint64_t log_file_write_time_us = 0;
    std::uint64_t max_log_file_write_time_us = 0;
    std::uint64_t log_file_flush_time_us = 0;
    std::uint64_t max_log_file_flush_time_us = 0;
    std::uint64_t oldest_log_record_age_us = 0;
    std::uint64_t max_log_record_age_us = 0;
    std::uint64_t log_writer_failures = 0;
    std::uint64_t log_writers_active = 0;
    std::uint64_t log_writer_healthy = 0;
    std::uint64_t max_log_batch_records = 0;
    std::uint64_t max_log_batch_bytes = 0;
    std::uint64_t log_rotations = 0;
    std::uint64_t log_retention_files_removed = 0;
    std::uint64_t log_retention_bytes_removed = 0;
    std::uint64_t current_log_storage_bytes = 0;
    std::uint64_t peak_log_storage_bytes = 0;
};

class RuntimeMetrics {
public:
    void connection_accepted(std::size_t current, std::size_t queued);
    void connection_rejected();
    void worker_started(std::size_t queued, std::uint64_t queue_wait_us);
    void worker_finished(std::size_t current);

    void request_started();
    void request_completed();
    void request_cancelled();
    void client_disconnected();
    void stream_started();
    void stream_chunk_forwarded(std::size_t bytes);

    void upstream_request_started();
    void upstream_request_completed();
    void upstream_request_cancelled();
    void upstream_request_failed();
    void upstream_timeout(UpstreamTimeoutPhase phase);
    void upstream_connection_handle_created();
    void upstream_request_handle_created();
    void upstream_bytes_sent(std::size_t bytes);
    void upstream_bytes_received(std::size_t bytes);
    void winhttp_connecting();
    void winhttp_connected();
    void winhttp_connection_closed();

    void log_record_enqueued(std::size_t records, std::size_t bytes, std::uint64_t oldest_pending_ns);
    void log_backpressure(std::uint64_t wait_us);
    void log_batch_written(
        std::size_t records,
        std::size_t bytes,
        std::uint64_t batch_wait_us,
        std::uint64_t write_us,
        std::uint64_t flush_us,
        std::uint64_t oldest_record_age_us,
        std::size_t pending_records,
        std::size_t pending_bytes,
        std::uint64_t oldest_pending_ns);
    void log_writer_started();
    void log_writer_failed(bool was_active);
    void log_writer_stopped();
    void log_rotated();
    void log_retention_removed(std::size_t files, std::uint64_t bytes);
    void log_storage_changed(std::uint64_t previous_bytes, std::uint64_t current_bytes);

    RuntimeMetricsSnapshot snapshot() const;

private:
    static void update_peak(std::atomic<std::uint64_t>& peak, std::uint64_t value);

    std::atomic<std::uint64_t> connections_accepted_{0};
    std::atomic<std::uint64_t> connections_rejected_{0};
    std::atomic<std::uint64_t> connections_completed_{0};
    std::atomic<std::uint64_t> current_connections_{0};
    std::atomic<std::uint64_t> peak_connections_{0};
    std::atomic<std::uint64_t> current_queued_connections_{0};
    std::atomic<std::uint64_t> peak_queued_connections_{0};
    std::atomic<std::uint64_t> current_active_workers_{0};
    std::atomic<std::uint64_t> peak_active_workers_{0};
    std::atomic<std::uint64_t> connection_queue_wait_time_us_{0};
    std::atomic<std::uint64_t> max_connection_queue_wait_us_{0};
    std::atomic<std::uint64_t> requests_started_{0};
    std::atomic<std::uint64_t> requests_completed_{0};
    std::atomic<std::uint64_t> requests_cancelled_{0};
    std::atomic<std::uint64_t> client_disconnects_{0};
    std::atomic<std::uint64_t> sse_streams_started_{0};
    std::atomic<std::uint64_t> stream_chunks_forwarded_{0};
    std::atomic<std::uint64_t> stream_bytes_forwarded_{0};
    std::atomic<std::uint64_t> upstream_requests_started_{0};
    std::atomic<std::uint64_t> upstream_requests_completed_{0};
    std::atomic<std::uint64_t> upstream_requests_cancelled_{0};
    std::atomic<std::uint64_t> upstream_requests_failed_{0};
    std::atomic<std::uint64_t> upstream_resolve_timeouts_{0};
    std::atomic<std::uint64_t> upstream_connect_timeouts_{0};
    std::atomic<std::uint64_t> upstream_send_timeouts_{0};
    std::atomic<std::uint64_t> upstream_response_header_timeouts_{0};
    std::atomic<std::uint64_t> upstream_stream_idle_timeouts_{0};
    std::atomic<std::uint64_t> upstream_response_body_timeouts_{0};
    std::atomic<std::uint64_t> upstream_total_timeouts_{0};
    std::atomic<std::uint64_t> upstream_connection_handles_created_{0};
    std::atomic<std::uint64_t> upstream_request_handles_created_{0};
    std::atomic<std::uint64_t> upstream_bytes_sent_{0};
    std::atomic<std::uint64_t> upstream_bytes_received_{0};
    std::atomic<std::uint64_t> winhttp_connecting_events_{0};
    std::atomic<std::uint64_t> winhttp_connected_events_{0};
    std::atomic<std::uint64_t> winhttp_connection_closed_events_{0};
    std::atomic<std::uint64_t> log_records_enqueued_{0};
    std::atomic<std::uint64_t> log_records_written_{0};
    std::atomic<std::uint64_t> log_bytes_written_{0};
    std::atomic<std::uint64_t> current_log_queue_records_{0};
    std::atomic<std::uint64_t> peak_log_queue_records_{0};
    std::atomic<std::uint64_t> current_log_queue_bytes_{0};
    std::atomic<std::uint64_t> peak_log_queue_bytes_{0};
    std::atomic<std::uint64_t> log_backpressure_count_{0};
    std::atomic<std::uint64_t> log_backpressure_wait_us_{0};
    std::atomic<std::uint64_t> log_batches_written_{0};
    std::atomic<std::uint64_t> log_flush_count_{0};
    std::atomic<std::uint64_t> log_write_time_us_{0};
    std::atomic<std::uint64_t> log_batch_wait_time_us_{0};
    std::atomic<std::uint64_t> max_log_batch_wait_us_{0};
    std::atomic<std::uint64_t> log_file_write_time_us_{0};
    std::atomic<std::uint64_t> max_log_file_write_time_us_{0};
    std::atomic<std::uint64_t> log_file_flush_time_us_{0};
    std::atomic<std::uint64_t> max_log_file_flush_time_us_{0};
    std::atomic<std::uint64_t> oldest_log_record_enqueued_ns_{0};
    std::atomic<std::uint64_t> max_log_record_age_us_{0};
    std::atomic<std::uint64_t> log_writer_failures_{0};
    std::atomic<std::uint64_t> log_writers_active_{0};
    std::atomic<std::uint64_t> max_log_batch_records_{0};
    std::atomic<std::uint64_t> max_log_batch_bytes_{0};
    std::atomic<std::uint64_t> log_rotations_{0};
    std::atomic<std::uint64_t> log_retention_files_removed_{0};
    std::atomic<std::uint64_t> log_retention_bytes_removed_{0};
    std::atomic<std::uint64_t> current_log_storage_bytes_{0};
    std::atomic<std::uint64_t> peak_log_storage_bytes_{0};
};

} // namespace ccs
