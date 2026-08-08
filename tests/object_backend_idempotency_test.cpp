#include "object/object_backend.h"
#include "object/request_fingerprint.h"
#include "storage/mkfs.h"

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <unistd.h>

namespace {

void Require(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
  }
}

std::string TemporaryPath() {
  std::array<char, 80> path_template{};
  std::strcpy(path_template.data(), "/tmp/eufs-idempotency-XXXXXX");
  const int fd = mkstemp(path_template.data());
  Require(fd >= 0, "mkstemp failed");
  close(fd);
  unlink(path_template.data());
  return path_template.data();
}

std::string CreateImage(std::uint32_t ledger_entries = 64) {
  eufs::storage::MkfsOptions options;
  options.image_path = TemporaryPath();
  options.image_size_bytes = 16ULL * 1024ULL * 1024ULL;
  options.total_inodes = 256;
  options.journal_blocks = 16;
  options.request_ledger_entries = ledger_entries;
  std::string detail;
  Require(eufs::storage::FormatImage(options, nullptr, &detail),
          detail.c_str());
  return options.image_path;
}

eufs::object_store::ObjectBackendOptions BackendOptions() {
  eufs::object_store::ObjectBackendOptions options;
  options.permissions = 0644;
  options.uid = 1000;
  options.gid = 1000;
  return options;
}

std::unique_ptr<eufs::object_store::ObjectBackend> OpenBackend(
    const std::string& path) {
  std::unique_ptr<eufs::object_store::ObjectBackend> backend;
  eufs::journal::RecoveryAction action{};
  std::string detail;
  Require(eufs::object_store::ObjectBackend::Open(
              path, BackendOptions(), &backend, &action, &detail) == 0,
          detail.c_str());
  return backend;
}

eufs::object_store::RequestIdentity Identity(
    std::uint8_t marker, eufs::object_store::MutationOperation operation,
    std::string_view key, std::string_view payload, std::uint64_t timestamp,
    eufs::object_store::ObjectVersion expected = {}) {
  eufs::object_store::RequestIdentity identity;
  identity.request_id.fill(marker);
  eufs::object_store::MutationIdentityInput input;
  input.operation = operation;
  input.key = key;
  input.payload_size = payload.size();
  input.payload_sha256.fill(marker);
  input.timestamp_ns = timestamp;
  input.expected_inode = expected.inode_number;
  input.expected_generation = expected.generation;
  std::string detail;
  Require(eufs::object_store::BuildRequestFingerprint(
              input, &identity.fingerprint, &detail) == 0,
          detail.c_str());
  return identity;
}

void TestSuccessReplayConflictAndRestart() {
  const std::string path = CreateImage();
  auto backend = OpenBackend(path);
  const auto identity = Identity(
      1, eufs::object_store::MutationOperation::kCreateIfAbsent, "alpha",
      "payload", 10);
  eufs::object_store::IdempotentMutationResult first;
  std::string detail;
  Require(backend->PutIfAbsentIdempotent("alpha", "payload", 10, identity,
                                         &first, &detail) == 0 &&
              first.disposition ==
                  eufs::object_store::RequestDisposition::kExecuted &&
              first.mutation.outcome ==
                  eufs::object_store::MutationOutcome::kCommitted,
          detail.c_str());

  eufs::object_store::IdempotentMutationResult replay;
  Require(backend->PutIfAbsentIdempotent(
              "alpha", "different-payload", 999, identity, &replay,
              &detail) == 0 &&
              replay.disposition ==
                  eufs::object_store::RequestDisposition::kReplayed &&
              replay.mutation.committed_version.inode_number ==
                  first.mutation.committed_version.inode_number &&
              replay.mutation.committed_version.generation ==
                  first.mutation.committed_version.generation,
          "same request identity did not replay its committed result");

  auto changed = identity;
  changed.fingerprint[0] ^= 1U;
  Require(backend->PutIfAbsentIdempotent(
              "alpha", "different-payload", 999, changed, &replay,
              &detail) == -EALREADY &&
              replay.disposition ==
                  eufs::object_store::RequestDisposition::kRequestIdConflict,
          "same request id with different fingerprint was executed");
  backend.reset();

  backend = OpenBackend(path);
  Require(backend->PutIfAbsentIdempotent("alpha", "payload", 10, identity,
                                         &replay, &detail) == 0 &&
              replay.disposition ==
                  eufs::object_store::RequestDisposition::kReplayed,
          "restart did not rebuild the persistent deduplication index");
  backend.reset();
  unlink(path.c_str());
}

void TestDeterministicRejectionsAreReplayable() {
  const std::string path = CreateImage();
  auto backend = OpenBackend(path);
  std::string detail;
  Require(backend->PutIfAbsent("existing", "old", 10, &detail) == 0,
          detail.c_str());

  auto create_identity = Identity(
      2, eufs::object_store::MutationOperation::kCreateIfAbsent, "existing",
      "new", 20);
  eufs::object_store::IdempotentMutationResult result;
  Require(backend->PutIfAbsentIdempotent("existing", "new", 20,
                                         create_identity, &result, &detail) ==
              -EEXIST &&
              result.mutation.outcome ==
                  eufs::object_store::MutationOutcome::kNotApplied,
          "EEXIST was not persisted as a deterministic rejection");
  Require(backend->PutIfAbsentIdempotent("existing", "new", 20,
                                         create_identity, &result, &detail) ==
              -EEXIST &&
              result.disposition ==
                  eufs::object_store::RequestDisposition::kReplayed,
          "EEXIST ledger record was not replayed");

  eufs::object_store::ObjectStat stat;
  Require(backend->Stat("existing", &stat, &detail) == 0, detail.c_str());
  const auto stale_identity = Identity(
      3, eufs::object_store::MutationOperation::kReplaceIfVersion, "existing",
      "replacement", 30,
      {stat.inode_number, stat.generation + 1U});
  Require(backend->ReplaceIfVersionIdempotent(
              "existing", {stat.inode_number, stat.generation + 1U},
              "replacement", 30, stale_identity, &result, &detail) ==
              -ESTALE,
          "ESTALE was not persisted as a deterministic rejection");
  Require(backend->ReplaceIfVersionIdempotent(
              "existing", {stat.inode_number, stat.generation + 1U},
              "replacement", 30, stale_identity, &result, &detail) ==
              -ESTALE &&
              result.disposition ==
                  eufs::object_store::RequestDisposition::kReplayed,
          "ESTALE ledger record was not replayed");

  const auto missing_identity = Identity(
      4, eufs::object_store::MutationOperation::kReplaceIfVersion,
      "missing", "replacement", 40, {77, 1});
  Require(backend->ReplaceIfVersionIdempotent(
              "missing", {77, 1}, "replacement", 40, missing_identity,
              &result, &detail) == -ENOENT &&
              backend->ReplaceIfVersionIdempotent(
                  "missing", {77, 1}, "replacement", 40, missing_identity,
                  &result, &detail) == -ENOENT &&
              result.disposition ==
                  eufs::object_store::RequestDisposition::kReplayed,
          "ENOENT ledger record was not replayed");
  backend.reset();

  backend = OpenBackend(path);
  Require(backend->ReplaceIfVersionIdempotent(
              "missing", {77, 1}, "replacement", 40, missing_identity,
              &result, &detail) == -ENOENT &&
              result.disposition ==
                  eufs::object_store::RequestDisposition::kReplayed,
          "restart did not decode and replay deterministic rejections");
  backend.reset();
  unlink(path.c_str());
}

void TestLedgerFullRejectsBeforeObjectPlanning() {
  const std::string path = CreateImage(32);
  auto backend = OpenBackend(path);
  std::string detail;
  Require(backend->PutIfAbsent("seed", "stable", 1, &detail) == 0,
          detail.c_str());
  for (std::uint8_t marker = 10; marker < 42; ++marker) {
    auto identity = Identity(
        marker, eufs::object_store::MutationOperation::kCreateIfAbsent,
        "seed", "other", marker);
    eufs::object_store::IdempotentMutationResult result;
    Require(backend->PutIfAbsentIdempotent("seed", "other", marker,
                                           identity, &result, &detail) ==
                -EEXIST,
            "could not fill ledger with deterministic results");
  }
  auto full_identity = Identity(
      99, eufs::object_store::MutationOperation::kCreateIfAbsent, "new-key",
      "must-not-write", 99);
  eufs::object_store::IdempotentMutationResult full;
  Require(backend->PutIfAbsentIdempotent("new-key", "must-not-write", 99,
                                         full_identity, &full, &detail) ==
              -ENOSPC &&
              full.disposition ==
                  eufs::object_store::RequestDisposition::kLedgerFull,
          "ledger-full request was not rejected before object planning");
  std::string contents;
  Require(backend->Get("new-key", &contents, &detail) == -ENOENT,
          "ledger-full request partially created an object");
  backend.reset();
  unlink(path.c_str());
}

}  // namespace

int main() {
  TestSuccessReplayConflictAndRestart();
  TestDeterministicRejectionsAreReplayable();
  TestLedgerFullRejectsBeforeObjectPlanning();
  std::cout << "PASS: Backend Request-ID replay, conflict, and ledger-full state machine\n";
  return 0;
}
