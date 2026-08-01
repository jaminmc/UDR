# Changelog

All notable changes to this fork ([jaminmc/UDR](https://github.com/jaminmc/UDR)) are documented here.

## [0.9.5] — 2026-08-01

### Added
- **IPv6 support** for rsync-style destinations (`user@[addr]:/path` and `[addr]:/path`)
- Dual-stack UDT sockets (`AF_UNSPEC` / IPv6 listen with dual-stack when available)
- **Automatic path MTU discovery** (`src/udr_mtu.cpp`) that sets `UDT_MSS` before connect/bind
- IPv4 vs IPv6 header-aware MSS conversion (IPv6 on a 1500 path uses ~1472, not 1500)
- WAN safety: **off-link peers capped at path MTU 1500** so local jumbo-frame NICs are not assumed end-to-end
- Jumbo (&gt;1500) only considered for on-link peers (same subnet, link-local, loopback)
- macOS autodetection of `os=OSX` and Homebrew OpenSSL under `/opt/homebrew` or `/usr/local`
- Verbose MTU logs (`-v`): `[udr mtu] … jumbo=no(wan-cap-1500)`

### Fixed
- Host parser treating the first `:` in an IPv6 address as the path separator
- UDT apps failing to compile on modern macOS/libc++ (`bind` vs `std::bind`)
- Default Linux/x86 `rdtsc` build failure on Apple Silicon when `os` was left at LINUX

### Changed
- README rewritten for this fork, with IPv6 and MTU guidance
- Prefer both peers on **0.9.5+** for IPv6 and correct packet sizing

## [0.9.4] and earlier

Upstream history from LabAdvComp/UDR and martinetd/UDR (OpenSSL 1.1+ fixes, security patches, etc.). See git history prior to this fork.
