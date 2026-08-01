UDR
===

**Version 0.9.5** · [![GitHub](https://img.shields.io/badge/github-jaminmc%2FUDR-blue)](https://github.com/jaminmc/UDR)

UDR is a wrapper around rsync that enables rsync to use [UDT](https://en.wikipedia.org/wiki/UDP-based_Data_Transfer_Protocol) for high-throughput transfers over high-latency networks.

This repository is a maintained fork of [martinetd/UDR](https://github.com/martinetd/UDR) (itself forked from [LabAdvComp/UDR](https://github.com/LabAdvComp/UDR)), with fixes for modern macOS (including Apple Silicon), OpenSSL via Homebrew, IPv6, and automatic path MTU / packet sizing.

> **Note:** For many long-distance TCP paths, enabling BBR congestion control may be enough and simpler than UDR:
> ```
> sysctl net.ipv4.tcp_congestion_control=bbr
> ```
> Use UDR when you specifically need UDT-based rsync over UDP.

WHAT'S NEW IN 0.9.5
-------------------

- **IPv6** — rsync-style hosts (`user@[2001:db8::1]:/path`), dual-stack UDT sockets
- **Path MTU autodetection** — sets `UDT_MSS` from the route (IPv4 vs IPv6 header-aware)
- **WAN jumbo safety** — local jumbo-frame NICs no longer inflate packet size for internet peers (capped at 1500 off-link)
- **macOS / Apple Silicon** — auto `os=OSX`, Homebrew OpenSSL (`/opt/homebrew` or `/usr/local`)
- **Build fixes** — modern libc++ `::bind` fix for UDT test apps

See [CHANGELOG.md](CHANGELOG.md) for the full list.

CONTENT
-------
```
./src     UDR source code
./udt     UDT source code, documentation and license
./server  Optional UDR server (Python)
./tests   Basic tests
```

BUILD
-----

### Dependencies
- A C++ compiler (`g++` / clang++)
- OpenSSL (`libssl` and `libcrypto`)
  - macOS (Homebrew): `brew install openssl`
  - Linux: install your distro’s `libssl-dev` / `openssl-devel` package

### Build

```bash
make
```

On macOS, `os=OSX` is detected automatically and OpenSSL is found under Homebrew (`/opt/homebrew` or `/usr/local`).

You can still override platform flags explicitly:

```bash
make -e os=XXX arch=YYY
```

| Variable | Values |
|----------|--------|
| `os`     | `LINUX` (default on non-Darwin), `BSD`, `OSX` |
| `arch`   | `AMD64` (default), `POWERPC`, `IA64`, `IA32` |

The binary is written to `src/udr`. Check the build with:

```bash
./src/udr --version
# UDR version v0.9.5
```

### Install

Works on **Linux, macOS, and BSD** (uses portable `install -d` / `install -m`):

```bash
sudo make install                 # → /usr/local/bin/udr
sudo make install PREFIX=/usr     # → /usr/bin/udr
make install DESTDIR=/tmp/stage   # staged install (packaging)
```

On **macOS only**, `make` and `make install` also ad-hoc `codesign` the binary. Do **not** use bare `sudo cp src/udr /usr/local/bin/` without re-signing — an invalid signature makes the kernel kill the process (`zsh: killed`). If that happens:

```bash
sudo codesign --force -s - /usr/local/bin/udr
```

On Linux/BSD, no code signing step is required.

USAGE
-----

UDR must be installed on **both** the client and server. It uses SSH for authentication and to start the remote UDR process. At least one UDP port must be open between the hosts (default range 9000–9100). Encryption is off by default; when enabled it uses OpenSSL (aes-128 by default).

### Basic usage

```bash
udr [udr options] rsync [rsync options] src dest
```

### UDR options

- `[-a starting port number]` default is 9000
- `[-b ending port number]` default is 9100
- `[-n aes-128 | aes-192 | aes-256 | bf | des-ede3]` enable encryption (default cipher aes-128 if `-n` has no argument)
- `[-p path]` local path for the `.udr_key` file used for encryption (default: current directory)
- `[-c remote udr location]` path to `udr` on the remote host (default: `udr` on `PATH`)
- `[-o server port]` port for UDR server mode (default 9000)
- `[-v]` verbose mode (includes MTU discovery logs)
- `[--version]` print version
- `[-d timeout]` idle timeout in seconds after connect with no data (default 15)
- `[-i ip]` interface address the remote process binds to (IPv4 or IPv6)
- `[-P ssh-port]` SSH port (default 22)

Do **not** pass rsync’s `-e` / `--rsh`; UDR supplies that itself.

### Examples

Local → remote (hostname):

```bash
udr rsync -av --stats --progress /home/user/tmp/ hostname.com:/home/user/tmp
```

With explicit remote `udr` path and UDP port range:

```bash
udr -c /home/user/udr/src/udr -a 8000 -b 8010 rsync -av --stats --progress \
  /home/user/tmp/ hostname.com:/home/user/tmp
```

**IPv6** (rsync-style bracket notation; **both ends need UDR ≥ 0.9.5**):

```bash
udr -v rsync -av --progress /local/path/ \
  'user@[2001:db8::1]:/remote/path/'
```

### Path MTU / packet size

UDR probes the path and sets `UDT_MSS` automatically before connecting:

- Converts path MTU → UDT MSS correctly for **IPv4 vs IPv6** (stock UDT `1500` assumes IPv4 headers; IPv6 needs a lower MSS on a 1500 MTU path, typically **1472**)
- **Internet / off-link peers are capped at 1500** even if your NIC has jumbo frames enabled — local jumbo does not exist on typical WAN paths
- Jumbo (&gt;1500) is only considered for on-link peers (same subnet, link-local, or loopback)
- DF-bit probes and OS hints can still *lower* MTU (PPPoE, tunnels, etc.)
- Handshake uses the minimum MSS of both peers
- With `-v`, logs include `jumbo=no(wan-cap-1500)` for internet destinations

### Notes

- After the transfer finishes, the local UDR thread is stopped by a signal. Rsync may print  
  `rsync error: sibling process terminated abnormally` — that message can be ignored if the transfer completed. Other rsync errors are real failures.
- Both ends must use a compatible `udr` binary (especially for IPv6 and MTU-aware MSS). Prefer **0.9.5+ on both sides**.

UDR SERVER
----------

The UDR server allows transfers for users without shell accounts (similar to rsync daemon mode). It is written in Python, listens on TCP, and launches rsync with “daemon features via a remote shell”. Requires UDR ≥ 0.9.2.

### Basic server usage

```bash
python udrserver.py [-v] [-s] [-c configfile] start|stop|restart|foreground
```

### UDR server options

- `[-c config file]` config path (default `/etc/udrd.conf`)
- `[-s]` silent mode
- `[-v]` verbose mode

### UDR server configuration

Example config is under `server/udrd.conf.example`. Parameters:

- `address`: IP to bind (default `0.0.0.0`)
- `server port`: TCP listen port (default 9000)
- `start port` / `end port`: UDP port range for UDR (default 9000–9100)
- `log file`, `log level`, `pid file`
- `udr`: path to `udr` binary
- `rsyncd conf`: rsyncd config file
- `uid` / `gid`: drop privileges when started as root
- `specify ip`: address for the UDR receiver to bind

Most standard `rsyncd.conf` options work. Known exceptions:

#### Max connections

`max connections` does not work reliably; limit concurrency via the UDP port range instead. If `max connections` is set, rsync may fail opening a lock file (`@ERROR: failed to open lock file`).

#### UID/GID and chroot

Prefer not to run the UDR server as root (chroot is then unavailable). For chroot, set `uid`/`gid` to root in `udrd.conf`; UDR uses the global uid/gid from `rsyncd.conf` for child processes (per-module uid/gid is not supported).

#### WARNING

UDR server has mainly been tested in **read-only** mode; enabling write access is not recommended.

### Connecting to the UDR server

Use double colons, as with rsync daemon mode:

```bash
udr rsync -av --stats --progress hostname.com::module/path/to/file /home/user/target
udr rsync hostname.com::
udr rsync hostname.com::module/path/to/file
```

HISTORY / LICENSE
-----------------

- Original project: [LabAdvComp/UDR](https://github.com/LabAdvComp/UDR)
- Intermediate fork (OpenSSL fixes): [martinetd/UDR](https://github.com/martinetd/UDR)
- This fork: [jaminmc/UDR](https://github.com/jaminmc/UDR) — v0.9.5: macOS/Apple Silicon, Homebrew OpenSSL, IPv6, path MTU autodetection

See `LICENSE.txt` and `udt/LICENSE.txt`.
