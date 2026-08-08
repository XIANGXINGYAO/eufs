#include "object/request_fingerprint.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <iostream>
#include <string>

namespace {

void Require(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
  }
}

eufs::object_store::MutationIdentityInput GoldenInput() {
  eufs::object_store::MutationIdentityInput input;
  input.operation = eufs::object_store::MutationOperation::kCreateIfAbsent;
  input.key = "a.bin";
  input.payload_size = 3;
  input.timestamp_ns = 7;
  for (std::size_t index = 0; index < input.payload_sha256.size(); ++index) {
    input.payload_sha256[index] = static_cast<std::uint8_t>(index);
  }
  return input;
}

void TestGoldenVectorAndDeterminism() {
  const eufs::object_store::RequestFingerprint expected{
      0xaa, 0x6f, 0xeb, 0x93, 0xe2, 0x7f, 0x2e, 0x49,
      0x2b, 0xc6, 0x8f, 0xe3, 0x76, 0xbd, 0x91, 0x4c,
      0x16, 0xbd, 0xd2, 0x40, 0xdb, 0xe3, 0x5b, 0x09,
      0x22, 0x62, 0x3d, 0xf7, 0x98, 0xf3, 0x21, 0xae,
  };
  eufs::object_store::RequestFingerprint first{};
  eufs::object_store::RequestFingerprint second{};
  std::string detail;
  const auto input = GoldenInput();
  Require(eufs::object_store::BuildRequestFingerprint(input, &first,
                                                       &detail) == 0 &&
              first == expected,
          "canonical fingerprint does not match the golden vector");
  Require(eufs::object_store::BuildRequestFingerprint(input, &second,
                                                       &detail) == 0 &&
              second == first,
          "same semantic request produced a different fingerprint");
}

void TestEverySemanticFieldAffectsIdentity() {
  const auto original = GoldenInput();
  eufs::object_store::RequestFingerprint baseline{};
  std::string detail;
  Require(eufs::object_store::BuildRequestFingerprint(
              original, &baseline, &detail) == 0,
          "could not build baseline fingerprint");

  auto RequireDifferent = [&](const auto& changed, const char* message) {
    eufs::object_store::RequestFingerprint fingerprint{};
    Require(eufs::object_store::BuildRequestFingerprint(
                changed, &fingerprint, &detail) == 0 &&
                fingerprint != baseline,
            message);
  };

  auto changed = original;
  changed.key = "b.bin";
  RequireDifferent(changed, "key did not affect fingerprint");
  changed = original;
  ++changed.payload_size;
  RequireDifferent(changed, "payload size did not affect fingerprint");
  changed = original;
  changed.payload_sha256[0] ^= 1U;
  RequireDifferent(changed, "payload digest did not affect fingerprint");
  changed = original;
  ++changed.timestamp_ns;
  RequireDifferent(changed, "timestamp did not affect fingerprint");
  changed = original;
  changed.operation =
      eufs::object_store::MutationOperation::kReplaceIfVersion;
  changed.expected_inode = 7;
  changed.expected_generation = 11;
  RequireDifferent(changed,
                   "operation and precondition did not affect fingerprint");
  changed.expected_inode = 8;
  RequireDifferent(changed, "expected inode did not affect fingerprint");
  changed.expected_inode = 7;
  changed.expected_generation = 12;
  RequireDifferent(changed, "expected generation did not affect fingerprint");
}

void TestInvalidInputDoesNotModifyOutput() {
  auto input = GoldenInput();
  input.expected_inode = 1;
  eufs::object_store::RequestFingerprint output{};
  output.fill(0xa5);
  const auto original = output;
  std::string detail;
  Require(eufs::object_store::BuildRequestFingerprint(input, &output,
                                                       &detail) == -EINVAL &&
              output == original && !detail.empty(),
          "invalid create changed output or returned the wrong error");

  input = GoldenInput();
  input.operation =
      eufs::object_store::MutationOperation::kReplaceIfVersion;
  Require(eufs::object_store::BuildRequestFingerprint(input, &output,
                                                       &detail) == -EINVAL &&
              output == original,
          "replace without a version was accepted");
}

}  // namespace

int main() {
  TestGoldenVectorAndDeterminism();
  TestEverySemanticFieldAffectsIdentity();
  TestInvalidInputDoesNotModifyOutput();
  std::cout << "request_fingerprint_test: PASS\n";
  return 0;
}
