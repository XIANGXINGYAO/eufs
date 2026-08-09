#include "object_service.pb.h"

#include <brpc/channel.h>
#include <brpc/controller.h>
#include <butil/time.h>
#include <gflags/gflags.h>
#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

DEFINE_string(server, "127.0.0.1:8027", "EUFS object server endpoint");
DEFINE_uint64(concurrency, 1, "Maximum number of in-flight Put RPCs");
DEFINE_uint64(requests, 1, "Total number of unique Put RPCs");
DEFINE_uint64(payload_size, 4096, "Payload bytes carried by every Put RPC");
DEFINE_string(key_prefix, "load-", "Prefix for unique object keys");
DEFINE_uint64(request_id_seed, 1, "Deterministic Request-ID namespace");
DEFINE_uint64(timestamp_base, 1, "First object timestamp in nanoseconds");
DEFINE_int32(timeout_ms, 30000, "Timeout for each Put RPC");
DEFINE_string(result_file, "", "TSV path for per-request results");

namespace {

using eufs::rpc::protocol::ObjectService_Stub;
using eufs::rpc::protocol::PutObjectRequest;
using eufs::rpc::protocol::PutObjectResponse;
using eufs::rpc::protocol::PutStatus;

// 所有发送线程先在这里集合，再同时开始，避免线程创建先后掩盖真实并发。
class StartGate {
 public:
  explicit StartGate(std::size_t participants) : participants_(participants) {}

  void ArriveAndWait() {
    std::unique_lock<std::mutex> lock(mutex_);
    ++arrived_;
    ready_.notify_all();
    ready_.wait(lock, [this] { return started_; });
  }

  void StartWhenReady() {
    std::unique_lock<std::mutex> lock(mutex_);
    ready_.wait(lock, [this] { return arrived_ == participants_; });
    started_ = true;
    ready_.notify_all();
  }

 private:
  const std::size_t participants_;
  std::mutex mutex_;
  std::condition_variable ready_;
  std::size_t arrived_{0};
  bool started_{false};
};

struct RequestResult {
  std::size_t index{0};
  std::string key;
  std::string request_id_hex;
  PutStatus status{eufs::rpc::protocol::PUT_STATUS_STORAGE_ERROR};
  bool rpc_failed{false};
  std::int64_t latency_us{0};
  std::string detail;
};

bool ComputeSha256(const std::string& payload, std::string* digest) {
  if (digest == nullptr) {
    return false;
  }
  digest->assign(EVP_MAX_MD_SIZE, '\0');
  unsigned int digest_size = 0;
  if (EVP_Digest(payload.data(), payload.size(),
                 reinterpret_cast<unsigned char*>(digest->data()),
                 &digest_size, EVP_sha256(), nullptr) != 1) {
    return false;
  }
  digest->resize(digest_size);
  return digest_size == 32;
}

void PutLe64(std::uint64_t value, std::uint8_t* output) {
  for (std::size_t index = 0; index < sizeof(value); ++index) {
    output[index] = static_cast<std::uint8_t>(value >> (index * 8U));
  }
}

// 压测 ID 只要求同一轮内确定且唯一，不承担随机数或身份认证用途。
std::array<std::uint8_t, 16> MakeRequestId(std::size_t index) {
  std::array<std::uint8_t, 16> request_id{};
  PutLe64(FLAGS_request_id_seed, request_id.data());
  PutLe64(static_cast<std::uint64_t>(index) + 1U,
          request_id.data() + sizeof(std::uint64_t));
  return request_id;
}

std::string Hex(const std::array<std::uint8_t, 16>& bytes) {
  std::ostringstream output;
  output << std::hex << std::setfill('0');
  for (const std::uint8_t byte : bytes) {
    output << std::setw(2) << static_cast<unsigned int>(byte);
  }
  return output.str();
}

std::int64_t Percentile(const std::vector<std::int64_t>& sorted,
                        std::size_t percentile) {
  if (sorted.empty()) {
    return 0;
  }
  const std::size_t index =
      ((sorted.size() - 1U) * std::min<std::size_t>(percentile, 100U)) / 100U;
  return sorted[index];
}

bool WriteResults(const std::vector<RequestResult>& results) {
  if (FLAGS_result_file.empty()) {
    return true;
  }
  std::ofstream output(FLAGS_result_file, std::ios::trunc);
  if (!output) {
    return false;
  }
  output << "index\tstatus\tkey\trequest_id\tlatency_us\tdetail\n";
  for (const auto& result : results) {
    output << result.index << '\t'
           << (result.rpc_failed
                   ? "RPC_ERROR"
                   : eufs::rpc::protocol::PutStatus_Name(result.status))
           << '\t' << result.key << '\t' << result.request_id_hex << '\t'
           << result.latency_us << '\t'
           << (result.detail.empty() ? "-" : result.detail) << '\n';
  }
  return static_cast<bool>(output);
}

}  // namespace

int main(int argc, char* argv[]) {
  GFLAGS_NAMESPACE::ParseCommandLineFlags(&argc, &argv, true);
  if (FLAGS_concurrency == 0 || FLAGS_requests == 0 ||
      FLAGS_concurrency > FLAGS_requests || FLAGS_payload_size == 0 ||
      FLAGS_payload_size > std::numeric_limits<std::size_t>::max() ||
      FLAGS_timeout_ms <= 0 || FLAGS_key_prefix.empty()) {
    std::cerr << "positive concurrency, requests, payload_size, timeout_ms, and "
                 "a nonempty key_prefix are required; concurrency must not "
                 "exceed requests\n";
    return 2;
  }

  const std::size_t concurrency =
      static_cast<std::size_t>(FLAGS_concurrency);
  const std::size_t request_count = static_cast<std::size_t>(FLAGS_requests);
  std::string payload(static_cast<std::size_t>(FLAGS_payload_size), 'P');
  std::string digest;
  if (!ComputeSha256(payload, &digest)) {
    std::cerr << "could not compute payload SHA-256\n";
    return 3;
  }

  brpc::Channel channel;
  brpc::ChannelOptions options;
  options.protocol = "baidu_std";
  options.timeout_ms = FLAGS_timeout_ms;
  options.max_retry = 0;
  if (channel.Init(FLAGS_server.c_str(), &options) != 0) {
    std::cerr << "could not initialize brpc channel\n";
    return 4;
  }

  ObjectService_Stub stub(&channel);
  // Channel 惰性建连；先用只读 Stat 建立连接，避免首次 TCP 握手污染并发 Put。
  eufs::rpc::protocol::StatObjectRequest warmup_request;
  eufs::rpc::protocol::StatObjectResponse warmup_response;
  brpc::Controller warmup_controller;
  warmup_request.set_key(".eufs-load-client-warmup");
  stub.StatObject(&warmup_controller, &warmup_request, &warmup_response,
                  nullptr);
  if (warmup_controller.Failed()) {
    std::cerr << "could not warm up brpc channel: "
              << warmup_controller.ErrorText() << '\n';
    return 5;
  }

  StartGate gate(concurrency);
  std::atomic<std::size_t> next_index{0};
  std::vector<RequestResult> results(request_count);
  std::vector<std::thread> workers;
  workers.reserve(concurrency);

  for (std::size_t worker = 0; worker < concurrency; ++worker) {
    workers.emplace_back([&] {
      gate.ArriveAndWait();
      for (;;) {
        const std::size_t index = next_index.fetch_add(1);
        if (index >= request_count) {
          return;
        }

        RequestResult& result = results[index];
        result.index = index;
        result.key = FLAGS_key_prefix + std::to_string(index);
        const auto request_id = MakeRequestId(index);
        result.request_id_hex = Hex(request_id);

        PutObjectRequest request;
        request.set_key(result.key);
        request.set_payload_size(payload.size());
        request.set_sha256(digest);
        request.set_timestamp_ns(FLAGS_timestamp_base + index);
        request.set_request_id(
            std::string(reinterpret_cast<const char*>(request_id.data()),
                        request_id.size()));
        request.mutable_create_if_absent();

        PutObjectResponse response;
        brpc::Controller controller;
        controller.request_attachment().append(payload);
        butil::Timer timer(butil::Timer::STARTED);
        stub.PutObject(&controller, &request, &response, nullptr);
        timer.stop();

        result.latency_us = timer.u_elapsed();
        if (controller.Failed()) {
          result.rpc_failed = true;
          result.detail = controller.ErrorText();
        } else {
          result.status = response.status();
          result.detail = response.detail();
        }
      }
    });
  }

  gate.StartWhenReady();
  for (auto& worker : workers) {
    worker.join();
  }

  std::size_t ok = 0;
  std::size_t overloaded = 0;
  std::size_t rpc_error = 0;
  std::size_t unexpected = 0;
  std::vector<std::int64_t> ok_latencies;
  std::vector<std::int64_t> overloaded_latencies;
  for (const auto& result : results) {
    if (result.rpc_failed) {
      ++rpc_error;
    } else if (result.status == eufs::rpc::protocol::PUT_STATUS_OK) {
      ++ok;
      ok_latencies.push_back(result.latency_us);
    } else if (result.status == eufs::rpc::protocol::PUT_STATUS_OVERLOADED) {
      ++overloaded;
      overloaded_latencies.push_back(result.latency_us);
    } else {
      ++unexpected;
    }
  }
  std::sort(ok_latencies.begin(), ok_latencies.end());
  std::sort(overloaded_latencies.begin(), overloaded_latencies.end());

  if (!WriteResults(results)) {
    std::cerr << "could not write result file: " << FLAGS_result_file << '\n';
    return 6;
  }

  std::cout << "requests=" << request_count << " concurrency=" << concurrency
            << " payload_size=" << payload.size() << " ok=" << ok
            << " overloaded=" << overloaded << " rpc_error=" << rpc_error
            << " unexpected=" << unexpected
            << " ok_latency_p50_us=" << Percentile(ok_latencies, 50)
            << " ok_latency_p95_us=" << Percentile(ok_latencies, 95)
            << " ok_latency_max_us="
            << (ok_latencies.empty() ? 0 : ok_latencies.back())
            << " overloaded_latency_p50_us="
            << Percentile(overloaded_latencies, 50)
            << " overloaded_latency_p95_us="
            << Percentile(overloaded_latencies, 95)
            << " overloaded_latency_max_us="
            << (overloaded_latencies.empty() ? 0
                                             : overloaded_latencies.back())
            << '\n';
  return rpc_error == 0 && unexpected == 0 ? 0 : 10;
}
