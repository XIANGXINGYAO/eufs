#!/usr/bin/env bash
set -euo pipefail

binary="${1:-./build/eufs}"
mount_dir="${2:-/tmp/eufs-truncate-mnt}"
log_file="${3:-/tmp/eufs-truncate.log}"

mkdir -p "$mount_dir"

cleanup() {
  fusermount3 -u "$mount_dir" 2>/dev/null || true
  if [[ -n "${eufs_pid:-}" ]]; then
    wait "$eufs_pid" 2>/dev/null || true
  fi
}
trap cleanup EXIT

umask 022
"$binary" -f "$mount_dir" >"$log_file" 2>&1 &
eufs_pid=$!

for _ in $(seq 1 50); do
  if mountpoint -q "$mount_dir"; then
    break
  fi
  sleep 0.1
done

mountpoint -q "$mount_dir"
printf 'hello' >"$mount_dir/a.txt"
printf 'hi' >"$mount_dir/a.txt"
test "$(cat "$mount_dir/a.txt")" = "hi"
test "$(stat -c %s "$mount_dir/a.txt")" = "2"

printf 'hello' >"$mount_dir/a.txt"
truncate -s 2 "$mount_dir/a.txt"
test "$(cat "$mount_dir/a.txt")" = "he"
test "$(stat -c %s "$mount_dir/a.txt")" = "2"

truncate -s 7 "$mount_dir/a.txt"
test "$(stat -c %s "$mount_dir/a.txt")" = "7"
actual_hex="$(od -An -tx1 -v "$mount_dir/a.txt" | tr -d ' \n')"
test "$actual_hex" = "68650000000000"

if truncate -s 0 "$mount_dir/hello.txt" 2>/dev/null; then
  printf 'readonly file unexpectedly allowed truncate\n' >&2
  exit 1
fi
test "$(cat "$mount_dir/hello.txt")" = "hello from eufs"

printf 'PASS: in-memory truncate smoke test\n'
