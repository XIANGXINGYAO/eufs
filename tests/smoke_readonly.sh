#!/usr/bin/env bash
set -euo pipefail

binary="${1:-./build/eufs}"
mount_dir="${2:-/tmp/eufs-mnt}"
log_file="${3:-/tmp/eufs-readonly.log}"

mkdir -p "$mount_dir"

cleanup() {
  fusermount3 -u "$mount_dir" 2>/dev/null || true
  if [[ -n "${eufs_pid:-}" ]]; then
    wait "$eufs_pid" 2>/dev/null || true
  fi
}
trap cleanup EXIT

"$binary" -f "$mount_dir" >"$log_file" 2>&1 &
eufs_pid=$!

for _ in $(seq 1 50); do
  if mountpoint -q "$mount_dir"; then
    break
  fi
  sleep 0.1
done

mountpoint -q "$mount_dir"
test -f "$mount_dir/hello.txt"
test -f "$mount_dir/note.txt"

listing="$(ls -1A "$mount_dir")"
test "$listing" = $'hello.txt\nnote.txt'

hello="$(cat "$mount_dir/hello.txt")"
test "$hello" = "hello from eufs"

note="$(cat "$mount_dir/note.txt")"
test "$note" = "eufs note"
test "$(stat -c %s "$mount_dir/note.txt")" = "10"

if cat "$mount_dir/missing.txt" >/dev/null 2>&1; then
  printf 'FAIL: missing path unexpectedly succeeded\n' >&2
  exit 1
fi

if printf 'changed\n' 2>/dev/null >"$mount_dir/note.txt"; then
  printf 'FAIL: write unexpectedly succeeded\n' >&2
  exit 1
fi
test "$(cat "$mount_dir/note.txt")" = "eufs note"

printf 'PASS: readonly FUSE smoke test\n'
