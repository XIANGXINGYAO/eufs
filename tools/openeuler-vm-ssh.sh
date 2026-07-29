#!/usr/bin/env bash
set -euo pipefail

vm_dir="${OE_VM_DIR:-$HOME/.local/share/openeuler-vm}"

exec ssh -tt \
  -p 2222 \
  -i "$vm_dir/id_ed25519" \
  -o GSSAPIAuthentication=no \
  -o StrictHostKeyChecking=yes \
  -o "UserKnownHostsFile=$vm_dir/known_hosts" \
  root@127.0.0.1
