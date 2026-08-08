#include "object_service.pb.h"

#include <brpc/channel.h>
#include <brpc/controller.h>
#include <butil/logging.h>
#include <gflags/gflags.h>

#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <iomanip>
#include <openssl/evp.h>
#include <sstream>
#include <string>

DEFINE_string(server, "127.0.0.1:8027", "EUFS object server endpoint");
DEFINE_string(operation, "put", "Operation: put, get, or stat");
DEFINE_string(key, "", "Object key");
DEFINE_string(payload, "", "Inline payload; ignored when --payload_file is set");
DEFINE_string(payload_file, "", "Binary payload file");
DEFINE_string(output_file, "", "GetObject binary output file");
DEFINE_uint64(timestamp_ns, 0, "Object mtime in nanoseconds");
DEFINE_uint32(expected_inode, 0, "Expected inode for conditional replace");
DEFINE_uint64(expected_generation, 0,
              "Expected generation for conditional replace");
DEFINE_int32(timeout_ms, 5000, "RPC timeout in milliseconds");

namespace {

bool ReadPayload(std::string* output) {
  if (FLAGS_payload_file.empty()) {
    *output = FLAGS_payload;
    return true;
  }
  std::ifstream input(FLAGS_payload_file, std::ios::binary);
  if (!input) {
    return false;
  }
  output->assign(std::istreambuf_iterator<char>(input),
                 std::istreambuf_iterator<char>());
  return input.good() || input.eof();
}

bool ComputeSha256(const std::string& payload, std::string* digest) {
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

std::string Hex(const std::string& bytes) {
  std::ostringstream output;
  output << std::hex << std::setfill('0');
  for (const unsigned char byte : bytes) {
    output << std::setw(2) << static_cast<unsigned int>(byte);
  }
  return output.str();
}

void PrintMetadata(const eufs::rpc::protocol::ObjectMetadata& metadata) {
  std::cout << " inode=" << metadata.version().inode_number()
            << " generation=" << metadata.version().generation()
            << " size=" << metadata.size()
            << " mtime_ns=" << metadata.mtime_ns();
}

}  // namespace

int main(int argc, char* argv[]) {
  GFLAGS_NAMESPACE::ParseCommandLineFlags(&argc, &argv, true);
  if (FLAGS_key.empty() || FLAGS_timeout_ms <= 0 ||
      (FLAGS_operation != "put" && FLAGS_operation != "get" &&
       FLAGS_operation != "stat")) {
    std::cerr << "--operation=put|get|stat, --key, and positive --timeout_ms "
                 "are required\n";
    return 2;
  }
  if (FLAGS_operation == "put" &&
      (FLAGS_expected_inode == 0) != (FLAGS_expected_generation == 0)) {
    std::cerr << "expected inode and generation must be set together\n";
    return 2;
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

  eufs::rpc::protocol::ObjectService_Stub stub(&channel);
  if (FLAGS_operation == "get") {
    eufs::rpc::protocol::GetObjectRequest request;
    eufs::rpc::protocol::GetObjectResponse response;
    brpc::Controller controller;
    request.set_key(FLAGS_key);
    stub.GetObject(&controller, &request, &response, nullptr);
    if (controller.Failed()) {
      std::cerr << "rpc_error=" << controller.ErrorCode()
                << " detail=" << controller.ErrorText() << '\n';
      return 5;
    }
    std::string payload;
    std::string digest;
    if (response.status() == eufs::rpc::protocol::READ_STATUS_OK &&
        (controller.response_attachment().copy_to(&payload) !=
             controller.response_attachment().size() ||
         !ComputeSha256(payload, &digest))) {
      std::cerr << "could not copy or hash GetObject attachment\n";
      return 6;
    }
    if (!FLAGS_output_file.empty() &&
        response.status() == eufs::rpc::protocol::READ_STATUS_OK) {
      std::ofstream output(FLAGS_output_file, std::ios::binary);
      output.write(payload.data(), static_cast<std::streamsize>(payload.size()));
      if (!output) {
        std::cerr << "could not write GetObject payload\n";
        return 7;
      }
    }
    std::cout << "status="
              << eufs::rpc::protocol::ReadStatus_Name(response.status());
    PrintMetadata(response.metadata());
    std::cout << " sha256=" << Hex(digest)
              << " detail=" << response.detail() << '\n';
    return response.status() == eufs::rpc::protocol::READ_STATUS_OK ? 0 : 10;
  }

  if (FLAGS_operation == "stat") {
    eufs::rpc::protocol::StatObjectRequest request;
    eufs::rpc::protocol::StatObjectResponse response;
    brpc::Controller controller;
    request.set_key(FLAGS_key);
    stub.StatObject(&controller, &request, &response, nullptr);
    if (controller.Failed()) {
      std::cerr << "rpc_error=" << controller.ErrorCode()
                << " detail=" << controller.ErrorText() << '\n';
      return 5;
    }
    std::cout << "status="
              << eufs::rpc::protocol::ReadStatus_Name(response.status());
    PrintMetadata(response.metadata());
    std::cout << " detail=" << response.detail() << '\n';
    return response.status() == eufs::rpc::protocol::READ_STATUS_OK ? 0 : 10;
  }

  std::string payload;
  std::string digest;
  if (!ReadPayload(&payload) || !ComputeSha256(payload, &digest)) {
    std::cerr << "could not read or hash payload\n";
    return 3;
  }
  eufs::rpc::protocol::PutObjectRequest request;
  eufs::rpc::protocol::PutObjectResponse response;
  brpc::Controller controller;
  request.set_key(FLAGS_key);
  request.set_payload_size(payload.size());
  request.set_sha256(digest);
  request.set_timestamp_ns(FLAGS_timestamp_ns);
  if (FLAGS_expected_inode == 0) {
    request.mutable_create_if_absent();
  } else {
    auto* expected = request.mutable_expected_version();
    expected->set_inode_number(FLAGS_expected_inode);
    expected->set_generation(FLAGS_expected_generation);
  }
  controller.request_attachment().append(payload);

  stub.PutObject(&controller, &request, &response, nullptr);
  if (controller.Failed()) {
    std::cerr << "rpc_error=" << controller.ErrorCode()
              << " detail=" << controller.ErrorText() << '\n';
    return 5;
  }

  std::cout << "status="
            << eufs::rpc::protocol::PutStatus_Name(response.status())
            << " committed_inode="
            << response.committed_version().inode_number()
            << " committed_generation="
            << response.committed_version().generation()
            << " current_inode=" << response.current_version().inode_number()
            << " current_generation="
            << response.current_version().generation()
            << " detail=" << response.detail() << '\n';
  return response.status() == eufs::rpc::protocol::PUT_STATUS_OK ? 0 : 10;
}
