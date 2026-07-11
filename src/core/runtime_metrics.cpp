#include "core/runtime_metrics.hpp"

#include <chrono>

namespace ccs {

namespace {

std::uint64_t load(const std::atomic<std::uint64_t>& value) {
    return value.load(std::memory_order_relaxed);
}

} // namespace

void RuntimeMetrics::update_peak(std::atomic<std::uint64_t>& peak, std::uint64_t value) {
    auto current = peak.load(std::memory_order_relaxed);
    while (current < value
        && !peak.compare_exchange_weak(current, value, std::memory_order_relaxed)) {
    }
}

void RuntimeMetrics::connection_accepted(std::size_t current, std::size_t queued) {
    connections_accepted_.fetch_add(1, std::memory_order_relaxed);
    current_connections_.store(current, std::memory_order_relaxed);
    current_queued_connections_.store(queued, std::memory_order_relaxed);
    update_peak(peak_connections_, current);
    update_peak(peak_queued_connections_, queued);
}

void RuntimeMetrics::connection_rejected() {
    connections_rejected_.fetch_add(1, std::memory_order_relaxed);
}

void RuntimeMetrics::worker_started(
    std::size_t queued,
    std::uint64_t queue_wait_us) {
    current_queued_connections_.store(queued, std::memory_order_relaxed);
    const auto active = current_active_workers_.fetch_add(1, std::memory_order_relaxed) + 1;
    update_peak(peak_active_workers_, active);
    connection_queue_wait_time_us_.fetch_add(queue_wait_us, std::memory_order_relaxed);
    update_peak(max_connection_queue_wait_us_, queue_wait_us);
}

void RuntimeMetrics::worker_finished(std::size_t current) {
    current_active_workers_.fetch_sub(1, std::memory_order_relaxed);
    current_connections_.store(current, std::memory_order_relaxed);
    connections_completed_.fetch_add(1, std::memory_order_relaxed);
}

void RuntimeMetrics::request_started() {
    requests_started_.fetch_add(1, std::memory_order_relaxed);
}

void RuntimeMetrics::request_completed() {
    requests_completed_.fetch_add(1, std::memory_order_relaxed);
}

void RuntimeMetrics::request_cancelled() {
    requests_cancelled_.fetch_add(1, std::memory_order_relaxed);
}

void RuntimeMetrics::client_disconnected() {
    client_disconnects_.fetch_add(1, std::memory_order_relaxed);
}

void RuntimeMetrics::stream_started() {
    sse_streams_started_.fetch_add(1, std::memory_order_relaxed);
}

void RuntimeMetrics::stream_chunk_forwarded(std::size_t bytes) {
    stream_chunks_forwarded_.fetch_add(1, std::memory_order_relaxed);
    stream_bytes_forwarded_.fetch_add(bytes, std::memory_order_relaxed);
}

void RuntimeMetrics::upstream_request_started() {
    upstream_requests_started_.fetch_add(1, std::memory_order_relaxed);
}

void RuntimeMetrics::upstream_request_completed() {
    upstream_requests_completed_.fetch_add(1, std::memory_order_relaxed);
}

void RuntimeMetrics::upstream_request_cancelled() {
    upstream_requests_cancelled_.fetch_add(1, std::memory_order_relaxed);
}

void RuntimeMetrics::upstream_request_failed() {
    upstream_requests_failed_.fetch_add(1, std::memory_order_relaxed);
}

void RuntimeMetrics::upstream_timeout(UpstreamTimeoutPhase phase) {
    switch (phase) {
    case UpstreamTimeoutPhase::Resolve:
        upstream_resolve_timeouts_.fetch_add(1, std::memory_order_relaxed);
        break;
    case UpstreamTimeoutPhase::Connect:
        upstream_connect_timeouts_.fetch_add(1, std::memory_order_relaxed);
        break;
    case UpstreamTimeoutPhase::Send:
        upstream_send_timeouts_.fetch_add(1, std::memory_order_relaxed);
        break;
    case UpstreamTimeoutPhase::ResponseHeader:
        upstream_response_header_timeouts_.fetch_add(1, std::memory_order_relaxed);
        break;
    case UpstreamTimeoutPhase::StreamIdle:
        upstream_stream_idle_timeouts_.fetch_add(1, std::memory_order_relaxed);
        break;
    case UpstreamTimeoutPhase::ResponseBody:
        upstream_response_body_timeouts_.fetch_add(1, std::memory_order_relaxed);
        break;
    case UpstreamTimeoutPhase::Total:
        upstream_total_timeouts_.fetch_add(1, std::memory_order_relaxed);
        break;
    }
}

void RuntimeMetrics::upstream_connection_handle_created() {
    upstream_connection_handles_created_.fetch_add(1, std::memory_order_relaxed);
}

void RuntimeMetrics::upstream_request_handle_created() {
    upstream_request_handles_created_.fetch_add(1, std::memory_order_relaxed);
}

void RuntimeMetrics::upstream_bytes_sent(std::size_t bytes) {
    upstream_bytes_sent_.fetch_add(bytes, std::memory_order_relaxed);
}

void RuntimeMetrics::upstream_bytes_received(std::size_t bytes) {
    upstream_bytes_received_.fetch_add(bytes, std::memory_order_relaxed);
}

void RuntimeMetrics::winhttp_connecting() {
    winhttp_connecting_events_.fetch_add(1, std::memory_order_relaxed);
}

void RuntimeMetrics::winhttp_connected() {
    winhttp_connected_events_.fetch_add(1, std::memory_order_relaxed);
}

void RuntimeMetrics::winhttp_connection_closed() {
    winhttp_connection_closed_events_.fetch_add(1, std::memory_order_relaxed);
}

void RuntimeMetrics::log_record_enqueued(
    std::size_t records,
    std::size_t bytes,
    std::uint64_t oldest_pending_ns) {
    log_records_enqueued_.fetch_add(1, std::memory_order_relaxed);
    current_log_queue_records_.store(records, std::memory_order_relaxed);
    current_log_queue_bytes_.store(bytes, std::memory_order_relaxed);
    oldest_log_record_enqueued_ns_.store(oldest_pending_ns, std::memory_order_relaxed);
    update_peak(peak_log_queue_records_, records);
    update_peak(peak_log_queue_bytes_, bytes);
}

void RuntimeMetrics::log_backpressure(std::uint64_t wait_us) {
    log_backpressure_count_.fetch_add(1, std::memory_order_relaxed);
    log_backpressure_wait_us_.fetch_add(wait_us, std::memory_order_relaxed);
}

void RuntimeMetrics::log_batch_written(
    std::size_t records,
    std::size_t bytes,
    std::uint64_t batch_wait_us,
    std::uint64_t write_us,
    std::uint64_t flush_us,
    std::uint64_t oldest_record_age_us,
    std::size_t pending_records,
    std::size_t pending_bytes,
    std::uint64_t oldest_pending_ns) {
    log_records_written_.fetch_add(records, std::memory_order_relaxed);
    log_bytes_written_.fetch_add(bytes, std::memory_order_relaxed);
    log_batches_written_.fetch_add(1, std::memory_order_relaxed);
    log_flush_count_.fetch_add(1, std::memory_order_relaxed);
    log_write_time_us_.fetch_add(write_us + flush_us, std::memory_order_relaxed);
    log_batch_wait_time_us_.fetch_add(batch_wait_us, std::memory_order_relaxed);
    log_file_write_time_us_.fetch_add(write_us, std::memory_order_relaxed);
    log_file_flush_time_us_.fetch_add(flush_us, std::memory_order_relaxed);
    current_log_queue_records_.store(pending_records, std::memory_order_relaxed);
    current_log_queue_bytes_.store(pending_bytes, std::memory_order_relaxed);
    oldest_log_record_enqueued_ns_.store(oldest_pending_ns, std::memory_order_relaxed);
    update_peak(max_log_batch_wait_us_, batch_wait_us);
    update_peak(max_log_file_write_time_us_, write_us);
    update_peak(max_log_file_flush_time_us_, flush_us);
    update_peak(max_log_record_age_us_, oldest_record_age_us);
    update_peak(max_log_batch_records_, records);
    update_peak(max_log_batch_bytes_, bytes);
}

void RuntimeMetrics::log_writer_started() {
    log_writer_healthy_.store(1, std::memory_order_relaxed);
}

void RuntimeMetrics::log_writer_failed() {
    log_writer_healthy_.store(0, std::memory_order_relaxed);
    log_writer_failures_.fetch_add(1, std::memory_order_relaxed);
}

void RuntimeMetrics::log_writer_stopped() {
    log_writer_healthy_.store(0, std::memory_order_relaxed);
}

RuntimeMetricsSnapshot RuntimeMetrics::snapshot() const {
    RuntimeMetricsSnapshot result;
#define CCS_LOAD_METRIC(name) result.name = load(name##_)
    CCS_LOAD_METRIC(connections_accepted);
    CCS_LOAD_METRIC(connections_rejected);
    CCS_LOAD_METRIC(connections_completed);
    CCS_LOAD_METRIC(current_connections);
    CCS_LOAD_METRIC(peak_connections);
    CCS_LOAD_METRIC(current_queued_connections);
    CCS_LOAD_METRIC(peak_queued_connections);
    CCS_LOAD_METRIC(current_active_workers);
    CCS_LOAD_METRIC(peak_active_workers);
    CCS_LOAD_METRIC(connection_queue_wait_time_us);
    CCS_LOAD_METRIC(max_connection_queue_wait_us);
    CCS_LOAD_METRIC(requests_started);
    CCS_LOAD_METRIC(requests_completed);
    CCS_LOAD_METRIC(requests_cancelled);
    CCS_LOAD_METRIC(client_disconnects);
    CCS_LOAD_METRIC(sse_streams_started);
    CCS_LOAD_METRIC(stream_chunks_forwarded);
    CCS_LOAD_METRIC(stream_bytes_forwarded);
    CCS_LOAD_METRIC(upstream_requests_started);
    CCS_LOAD_METRIC(upstream_requests_completed);
    CCS_LOAD_METRIC(upstream_requests_cancelled);
    CCS_LOAD_METRIC(upstream_requests_failed);
    CCS_LOAD_METRIC(upstream_resolve_timeouts);
    CCS_LOAD_METRIC(upstream_connect_timeouts);
    CCS_LOAD_METRIC(upstream_send_timeouts);
    CCS_LOAD_METRIC(upstream_response_header_timeouts);
    CCS_LOAD_METRIC(upstream_stream_idle_timeouts);
    CCS_LOAD_METRIC(upstream_response_body_timeouts);
    CCS_LOAD_METRIC(upstream_total_timeouts);
    CCS_LOAD_METRIC(upstream_connection_handles_created);
    CCS_LOAD_METRIC(upstream_request_handles_created);
    CCS_LOAD_METRIC(upstream_bytes_sent);
    CCS_LOAD_METRIC(upstream_bytes_received);
    CCS_LOAD_METRIC(winhttp_connecting_events);
    CCS_LOAD_METRIC(winhttp_connected_events);
    CCS_LOAD_METRIC(winhttp_connection_closed_events);
    CCS_LOAD_METRIC(log_records_enqueued);
    CCS_LOAD_METRIC(log_records_written);
    CCS_LOAD_METRIC(log_bytes_written);
    CCS_LOAD_METRIC(current_log_queue_records);
    CCS_LOAD_METRIC(peak_log_queue_records);
    CCS_LOAD_METRIC(current_log_queue_bytes);
    CCS_LOAD_METRIC(peak_log_queue_bytes);
    CCS_LOAD_METRIC(log_backpressure_count);
    CCS_LOAD_METRIC(log_backpressure_wait_us);
    CCS_LOAD_METRIC(log_batches_written);
    CCS_LOAD_METRIC(log_flush_count);
    CCS_LOAD_METRIC(log_write_time_us);
    CCS_LOAD_METRIC(log_batch_wait_time_us);
    CCS_LOAD_METRIC(max_log_batch_wait_us);
    CCS_LOAD_METRIC(log_file_write_time_us);
    CCS_LOAD_METRIC(max_log_file_write_time_us);
    CCS_LOAD_METRIC(log_file_flush_time_us);
    CCS_LOAD_METRIC(max_log_file_flush_time_us);
    const auto oldest_enqueued_ns = load(oldest_log_record_enqueued_ns_);
    if (oldest_enqueued_ns != 0) {
        const auto now_ns = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
        result.oldest_log_record_age_us = now_ns > oldest_enqueued_ns
            ? (now_ns - oldest_enqueued_ns) / 1000
            : 0;
    }
    CCS_LOAD_METRIC(max_log_record_age_us);
    CCS_LOAD_METRIC(log_writer_failures);
    CCS_LOAD_METRIC(log_writer_healthy);
    CCS_LOAD_METRIC(max_log_batch_records);
    CCS_LOAD_METRIC(max_log_batch_bytes);
#undef CCS_LOAD_METRIC

    return result;
}

} // namespace ccs
