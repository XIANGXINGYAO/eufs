#include "rpc/object_service_metrics.h"

#include <bvar/bvar.h>

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

namespace {

void Require(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
  }
}

void RequireMetric(const char* name, const char* expected) {
  const std::string actual = bvar::Variable::describe_exposed(name);
  if (actual != expected) {
    std::cerr << "FAIL: " << name << " expected=" << expected
              << " actual=" << actual << '\n';
    std::exit(1);
  }
}

void TestCountersLatenciesAndLiveGauges() {
  eufs::rpc::InflightByteLimiter limiter(128);
  eufs::rpc::BoundedTaskQueue write_queue(2);
  eufs::rpc::BoundedTaskQueue read_queue(2);

  {
    eufs::rpc::ObjectServiceMetrics metrics(&limiter, &write_queue,
                                             &read_queue);
    RequireMetric("eufs_object_service_inflight_payload_bytes", "0");
    RequireMetric("eufs_object_service_write_queue_depth", "0");
    RequireMetric("eufs_object_service_read_queue_depth", "0");

    auto lease = limiter.TryAcquire(32);
    Require(lease.has_value(), "could not acquire gauge test byte lease");
    eufs::rpc::QueuedTask write_task([] {}, [] {});
    eufs::rpc::QueuedTask read_task([] {}, [] {});
    Require(write_queue.TryEnqueue(&write_task) ==
                eufs::rpc::EnqueueResult::kAccepted,
            "could not enqueue write gauge test task");
    Require(read_queue.TryEnqueue(&read_task) ==
                eufs::rpc::EnqueueResult::kAccepted,
            "could not enqueue read gauge test task");
    RequireMetric("eufs_object_service_inflight_payload_bytes", "32");
    RequireMetric("eufs_object_service_write_queue_depth", "1");
    RequireMetric("eufs_object_service_read_queue_depth", "1");

    metrics.RecordWriteQueueWait(11);
    metrics.RecordReadQueueWait(12);
    metrics.RecordPutExecution(13);
    metrics.RecordGetExecution(14);
    metrics.RecordStatExecution(15);
    metrics.RecordInflightByteRejection();
    metrics.RecordWriteQueueRejection();
    metrics.RecordReadQueueRejection();
    metrics.RecordQueueAllocationFailure();
    metrics.RecordRequestReplay();
    metrics.RecordRequestIdConflict();
    metrics.RecordLedgerFull();
    metrics.RecordResultUnknown();
    metrics.RecordStorageError();

    RequireMetric("eufs_object_service_write_queue_wait_count", "1");
    RequireMetric("eufs_object_service_read_queue_wait_count", "1");
    RequireMetric("eufs_object_service_put_execution_count", "1");
    RequireMetric("eufs_object_service_get_execution_count", "1");
    RequireMetric("eufs_object_service_stat_execution_count", "1");

    // LatencyRecorder 的窗口值由后台统计周期刷新，count 则会立即更新。
    std::this_thread::sleep_for(std::chrono::seconds(1));
    RequireMetric("eufs_object_service_write_queue_wait_latency", "11");
    RequireMetric("eufs_object_service_read_queue_wait_latency", "12");
    RequireMetric("eufs_object_service_put_execution_latency", "13");
    RequireMetric("eufs_object_service_get_execution_latency", "14");
    RequireMetric("eufs_object_service_stat_execution_latency", "15");
    RequireMetric("eufs_object_service_inflight_byte_rejection_count", "1");
    RequireMetric("eufs_object_service_write_queue_rejection_count", "1");
    RequireMetric("eufs_object_service_read_queue_rejection_count", "1");
    RequireMetric("eufs_object_service_queue_allocation_failure_count", "1");
    RequireMetric("eufs_object_service_request_replay_count", "1");
    RequireMetric("eufs_object_service_request_id_conflict_count", "1");
    RequireMetric("eufs_object_service_ledger_full_count", "1");
    RequireMetric("eufs_object_service_result_unknown_count", "1");
    RequireMetric("eufs_object_service_storage_error_count", "1");
  }

  Require(bvar::Variable::describe_exposed(
              "eufs_object_service_inflight_payload_bytes")
              .empty(),
          "destroying metrics left a stale exposed gauge");
}

}  // namespace

int main() {
  TestCountersLatenciesAndLiveGauges();
  std::cout << "object_service_metrics_test: PASS\n";
  return 0;
}
