#!/usr/bin/env bash
set -euo pipefail

binary="${1:-./build/eufs}"
mount_dir="${2:-/tmp/eufs-create-mnt}"
log_file="${3:-/tmp/eufs-create.log}"

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
: >"$mount_dir/a.txt"

test -f "$mount_dir/a.txt"
test "$(stat -c %a "$mount_dir/a.txt")" = "644"
test "$(stat -c %s "$mount_dir/a.txt")" = "0"
test -z "$(cat "$mount_dir/a.txt")"

listing="$(ls -1A "$mount_dir")"
test "$listing" = $'a.txt\nhello.txt\nnote.txt'

test "$(cat "$mount_dir/hello.txt")" = "hello from eufs"
test "$(cat "$mount_dir/note.txt")" = "eufs note"

printf 'PASS: in-memory create smoke test\n'
