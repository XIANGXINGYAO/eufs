#!/usr/bin/env bash
set -euo pipefail

mkfs_binary="${1:-./build/eufs-mkfs}"
daemon_binary="${2:-./build/eufsd}"
image="${3:-/tmp/eufs-disk-root.img}"
mount_dir="${4:-/tmp/eufs-disk-root-mnt}"
log_file="${5:-/tmp/eufs-disk-root.log}"

mkdir -p "$mount_dir"

cleanup() {
  fusermount3 -u "$mount_dir" 2>/dev/null || true
  if [[ -n "${daemon_pid:-}" ]]; then
    wait "$daemon_pid" 2>/dev/null || true
  fi
}
trap cleanup EXIT

"$mkfs_binary" --image "$image" --size 8M --inodes 128 \
  --journal-blocks 16 --force >/dev/null
before_digest="$(sha256sum "$image" | cut -d' ' -f1)"

mount_and_check() {
  cycle="$1"
  "$daemon_binary" --image "$image" -f -o ro "$mount_dir" \
    >"$log_file" 2>&1 &
  daemon_pid=$!
  for _ in $(seq 1 50); do
    if mountpoint -q "$mount_dir"; then
      break
    fi
    sleep 0.1
  done
  mountpoint -q "$mount_dir"
  root_inode="$(stat -c %i "$mount_dir")"
  root_links="$(stat -c %h "$mount_dir")"
  root_mode="$(stat -c %a "$mount_dir")"
  test "$root_inode" = "1"
  test "$root_links" = "2"
  test "$root_mode" = "755"
  test -z "$(ls -1A "$mount_dir")"
  if touch "$mount_dir/should-fail" 2>/dev/null; then
    printf 'read-only disk-backed mount accepted create\n' >&2
    exit 1
  fi
  printf 'cycle=%s root_inode=%s root_links=%s root_mode=%s\n' \
    "$cycle" "$root_inode" "$root_links" "$root_mode"
  fusermount3 -u "$mount_dir"
  wait "$daemon_pid"
  unset daemon_pid
}

mount_and_check 1
mount_and_check 2

after_digest="$(sha256sum "$image" | cut -d' ' -f1)"
test "$before_digest" = "$after_digest"
printf 'image_sha256_before=%s\nimage_sha256_after=%s\n' \
  "$before_digest" "$after_digest"

printf 'PASS: disk-backed root mount and remount smoke test\n'
