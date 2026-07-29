#!/usr/bin/env bash
set -euo pipefail

binary="${1:-./build/eufs}"
mount_dir="${2:-/tmp/eufs-write-mnt}"
log_file="${3:-/tmp/eufs-write.log}"

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
test "$(cat "$mount_dir/a.txt")" = "hello"
test "$(stat -c %s "$mount_dir/a.txt")" = "5"

printf 'XY' | dd of="$mount_dir/a.txt" bs=1 seek=1 conv=notrunc status=none
test "$(cat "$mount_dir/a.txt")" = "hXYlo"
test "$(stat -c %s "$mount_dir/a.txt")" = "5"

printf 'XY' | dd of="$mount_dir/a.txt" bs=1 seek=7 conv=notrunc status=none
test "$(stat -c %s "$mount_dir/a.txt")" = "9"
actual_hex="$(od -An -tx1 -v "$mount_dir/a.txt" | tr -d ' \n')"
test "$actual_hex" = "6858596c6f00005859"

test "$(cat "$mount_dir/hello.txt")" = "hello from eufs"
test "$(cat "$mount_dir/note.txt")" = "eufs note"

printf 'PASS: in-memory write smoke test\n'
