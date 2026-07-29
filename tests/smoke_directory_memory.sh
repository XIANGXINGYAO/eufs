#!/usr/bin/env bash
set -euo pipefail

binary="${1:-./build/eufs}"
mount_dir="${2:-/tmp/eufs-directory-mnt}"
log_file="${3:-/tmp/eufs-directory.log}"

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
test "$(stat -c %h "$mount_dir")" = "2"

mkdir "$mount_dir/d"
test -d "$mount_dir/d"
test "$(stat -c %h "$mount_dir")" = "3"
test "$(stat -c %h "$mount_dir/d")" = "2"

printf 'nested-content' >"$mount_dir/d/a.txt"
test "$(cat "$mount_dir/d/a.txt")" = "nested-content"
mkdir "$mount_dir/d/sub"
test "$(stat -c %h "$mount_dir/d")" = "3"
test "$(stat -c %h "$mount_dir/d/sub")" = "2"

if rmdir "$mount_dir/d" 2>/dev/null; then
  printf 'non-empty directory unexpectedly removed\n' >&2
  exit 1
fi

mv "$mount_dir/d/a.txt" "$mount_dir/d/sub/moved.txt"
test ! -e "$mount_dir/d/a.txt"
test "$(cat "$mount_dir/d/sub/moved.txt")" = "nested-content"

rm "$mount_dir/d/sub/moved.txt"
rmdir "$mount_dir/d/sub"
test "$(stat -c %h "$mount_dir/d")" = "2"
rmdir "$mount_dir/d"
test ! -e "$mount_dir/d"
test "$(stat -c %h "$mount_dir")" = "2"

printf 'PASS: in-memory directory smoke test\n'
