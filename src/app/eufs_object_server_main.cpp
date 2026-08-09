#include "journal/journal_control_store.h"
#include "journal/durable_stage_failpoint.h"
#include "object/object_backend.h"
#include "rpc/object_service_impl.h"

#include <brpc/server.h>
#include <butil/endpoint.h>
#include <butil/logging.h>
#include <gflags/gflags.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

DEFINE_string(image, "", "Path to the EUFS image");
DEFINE_string(listen_addr, "127.0.0.1:8027", "brpc listen endpoint");
DEFINE_uint64(max_inflight_bytes, 64U * 1024U * 1024U,
              "Maximum admitted PutObject payload bytes");
DEFINE_uint64(max_queued_write_tasks, 128,
              "Maximum queued PutObject requests");
DEFINE_uint64(max_queued_read_tasks, 256,
              "Maximum queued GetObject and StatObject requests");
DEFINE_uint64(read_workers, 4, "Number of bounded blocking read workers");
DEFINE_string(crash_after, "",
              "Test-only durable stage that terminates the server after sync");

int main(int argc, char* argv[]) {
  GFLAGS_NAMESPACE::ParseCommandLineFlags(&argc, &argv, true);
  if (FLAGS_image.empty() || FLAGS_max_queued_write_tasks == 0 ||
      FLAGS_max_queued_read_tasks == 0 || FLAGS_read_workers == 0) {
    LOG(ERROR) << "--image and positive queue/worker limits are required";
    return 2;
  }

  eufs::object_store::ObjectBackendOptions backend_options;
  std::shared_ptr<eufs::journal::DurableStageObserver> mutation_observer;
  if (!FLAGS_crash_after.empty()) {
    eufs::journal::DurableStage stage{};
    if (!eufs::journal::ParseDurableStage(FLAGS_crash_after, &stage)) {
      LOG(ERROR) << "invalid --crash_after: " << FLAGS_crash_after;
      return 2;
    }
    mutation_observer = eufs::journal::MakeProcessCrashObserver(
        stage, "eufs_object_server");
  }
  std::unique_ptr<eufs::object_store::ObjectBackend> backend;
  eufs::journal::RecoveryAction recovery_action{};
  std::string detail;
  const int open_result = eufs::object_store::ObjectBackend::Open(
      FLAGS_image, backend_options, &backend, &recovery_action, &detail,
      nullptr, nullptr, std::move(mutation_observer));
  if (open_result != 0) {
    LOG(ERROR) << "ObjectBackend open failed: " << detail;
    return 3;
  }

  eufs::rpc::ObjectServiceOptions service_options;
  service_options.max_inflight_bytes =
      static_cast<std::size_t>(FLAGS_max_inflight_bytes);
  service_options.max_queued_write_tasks =
      static_cast<std::size_t>(FLAGS_max_queued_write_tasks);
  service_options.max_queued_read_tasks =
      static_cast<std::size_t>(FLAGS_max_queued_read_tasks);
  service_options.read_worker_count =
      static_cast<std::size_t>(FLAGS_read_workers);
  eufs::rpc::ObjectServiceImpl service(backend.get(), service_options);

  brpc::Server server;
  if (server.AddService(&service, brpc::SERVER_DOESNT_OWN_SERVICE) != 0) {
    LOG(ERROR) << "could not add EUFS object service";
    return 4;
  }
  butil::EndPoint endpoint;
  if (butil::str2endpoint(FLAGS_listen_addr.c_str(), &endpoint) != 0) {
    LOG(ERROR) << "invalid --listen_addr: " << FLAGS_listen_addr;
    return 5;
  }

  brpc::ServerOptions server_options;
  // bthread 是进程级线程池；交给 --bthread_concurrency 统一配置。小型 VM 上
  // ServerOptions 的 ncore+1 默认值可能低于已初始化线程数，brpc 不支持缩容。
  server_options.num_threads = 0;
  if (server.Start(endpoint, &server_options) != 0) {
    LOG(ERROR) << "could not start EUFS object server";
    return 6;
  }
  server.RunUntilAskedToQuit();
  service.Shutdown();
  return 0;
}
