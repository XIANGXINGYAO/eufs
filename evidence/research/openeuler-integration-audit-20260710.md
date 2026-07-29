# openEuler integration audit

Audit date: 2026-07-10

## Decision

The project keeps the C++/libfuse crash-consistency core and adds a mandatory
openEuler sysSentry integration. It does not switch to EulerFS and does not add
A-Tune or gala-gopher to the implementation.

## Verified official artifacts

### openEuler installation target

- Release directory: `https://repo.openeuler.org/openEuler-24.03-LTS-SP4/`
- Netinstall ISO: `openEuler-24.03-LTS-SP4-netinst-x86_64-dvd.iso`
- Published size: 1.3 GiB
- Published SHA-256:
  `de7ee073c0708e5da88b08bcbdaad27c3f25d302a3507eec4238ca19fc21a396`

The SP4 x86_64 package lists expose these exact packages:

```text
fuse3-3.16.2-3.oe2403sp4.x86_64
fuse3-devel-3.16.2-3.oe2403sp4.x86_64
sysSentry-1.0.3-46.oe2403sp4.x86_64
libxalarm-1.0.3-46.oe2403sp4.x86_64
libxalarm-devel-1.0.3-46.oe2403sp4.x86_64
```

Sources checked:

- `https://repo.openeuler.org/openEuler-24.03-LTS-SP4/ISO/x86_64/`
- `https://repo.openeuler.org/openEuler-24.03-LTS-SP4/everything/x86_64/Packages/`

### sysSentry

- Main repository: `https://atomgit.com/openeuler/sysSentry`
- Audited HEAD: `03d523063880193efb1861ca7ff91ff8f45e8175`
- HEAD date: 2026-07-09
- C result API: `src/libs/libxalarm/register_xalarm.h`
- Implementation: `src/libs/libxalarm/register_xalarm.c`
- Result socket: `/var/run/sysSentry/result.sock`
- C++ plugin precedent:
  `src/sentryPlugins/bmc_ras_sentry/src/cbmcrassentry.cpp`
- Task configuration precedent: `config/tasks/*.mod`

The adapter may map a checker result to `RESULT_LEVEL_*` and call
`report_result()`. It must not contain a second implementation of filesystem
consistency rules.

### Distribution packaging

- Package repository: `https://atomgit.com/src-openeuler/sysSentry`
- Audited commit: `c9c43a4`
- The spec defines `sysSentry`, `libxalarm`, and `libxalarm-devel` subpackages.
- `libxalarm-devel` installs `xalarm/register_xalarm.h`.

### openEuler fuse3

- Package repository: `https://atomgit.com/src-openeuler/fuse3`
- Audited HEAD: `4591869`
- Spec version: `3.16.2-3`
- The package carries openEuler-maintained libfuse patches. Cross-distribution
  behavior must be measured by the same test matrix, not inferred from the
  patch list.

### Rejected direct base: EulerFS

- Repository: `https://atomgit.com/openeuler/eulerfs`
- Audited HEAD: `ac6b03f`
- Last source-history date: 2022-07-11
- Size in the audited tree: about 9,642 lines of C and headers.
- Requirements: Linux kernel module, 5.10-era APIs, LIBNVDIMM, PMEM, and DAX.

EulerFS is a real openEuler filesystem but is not a feasible replacement for
the current C++ userspace route under the July schedule.

## Current execution environment

```text
OS: Ubuntu 20.04 VMware guest
CPU allocation: 2 vCPU
RAM: 5.9 GiB
Free root-disk space at audit: 5.8 GiB
/dev/kvm: unavailable
QEMU/Podman/Docker at initial audit: unavailable
passwordless sudo: unavailable
VMware shared folders: none configured
```

The initial environment audit did not prove that this guest could host a nested
openEuler VM. Subsequent execution invalidated the separate-VM requirement:
QEMU 4.2.1 was installed without root under the user's home directory, and the
official SP4 qcow2 image booted successfully with TCG. OE0 then passed in that
local VM. The raw runtime record is
`evidence/runtime/oe0-runtime-raw-20260710.log`.

## Claim boundary

The SP4 runtime baseline is now earned: Stage A builds, mounts, reads, and
unmounts on SP4, while xalarmd/sysSentry and their sockets are active. This is
environment evidence only. The openEuler project claim remains unsupported
until the real shared consistency checker is exposed through sysSentry, with
raw task-result and RPM lifecycle evidence.
