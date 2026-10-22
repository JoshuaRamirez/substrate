# Changelog

Every release of substrate. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and releases follow
[Semantic Versioning](https://semver.org/). A release number is not the
segment's format number: `SUB_RELEASE` counts what ships, and `SUB_VERSION`
(8) pins the bytes of the segment.

## [0.2.0] - 2026-10-22

Another machine: each machine's registry, process control and events are
reachable, live, from the machines it trusts.
Baseline `af6f27e` (0.1.0). Commits `af6f27e..` the 0.2.0 merge on `main`.
Tests: full suite (test.sh, desk-heavy nice -n 10, macOS, load average about 60): 256 pass, 3 skip (swiftc twice, docker), 2 fail, both the contention storm's load-bound "timeouts stay under 1%" at 64 and 128 clients (its correctness checks, no duplicate ids and every gap an admitted timeout, pass; the ring is unchanged in this release); test-net.sh 33/33; sub-net selftest 14/14. Notes: [docs/releases/v0.2.0.md](docs/releases/v0.2.0.md).

### Added
- `sub_net_transport`, the network seam in `substrate.h`, and the protocol
  above it (one JSON object a frame).
- `sub-net` (`bin/net`, `net.c`): `keygen`, `trust`, `peers`, `serve`, `ps`,
  `run`, `stop`, `enable`, `disable`, `events` (pushed live), `listen` and
  `connect` (named line channels), `selftest`, `--version`.
- The transport `Noise_IK_25519_ChaChaPoly_SHA256` over TCP: mutual
  authentication by per-machine X25519 keys, a separate key per direction,
  forward secrecy.
- `sub net …`, `sub keygen`, `sub --version`.
- `test-net.sh`, run by `test.sh`: two substrates on one Mac over 127.0.0.1.
- `SUB_RELEASE` in `substrate.h`.
- FEATURES.md, this changelog, and `docs/releases/`.

### Changed
- README: *Another machine* now describes the adapter, not a boundary.
- `make install` / `uninstall` cover `sub-net`.

### Security
- The cipher suite (SHA-256, HMAC-SHA-256, ChaCha20, Poly1305,
  ChaCha20-Poly1305, X25519) is implemented in-tree in `net.c`, with no
  third-party code. `sub-net selftest` checks it against FIPS 180-2, RFC
  4231, RFC 8439 and RFC 7748 vectors, and the suite runs it.
- `sub-net serve` is refused any address that is not loopback or private:
  `0.0.0.0`, `::` and public addresses never listen.
- Keys live outside the repo, the secret half has mode 0600, and no key is
  printed or logged. The suite checks this.

## [0.1.0] - before 2026-10-22

The baseline, untagged, at `af6f27e`: the segment (format v8) with its peer
table, ring, arena, blob store, admin token and live database path; `dbd`,
`sub` (`run`, `ps`, `stop`, `enable` / `disable` / `enabled`, `restart`,
`change` / `changes`), `webd`, `look`, `layout`, `bench`, `gui`, `top`,
`peer`, `pypeer.py` and `swiftpeer`; macOS and Linux. Full suite: 224 pass.

[0.2.0]: docs/releases/v0.2.0.md
[0.1.0]: docs/releases/INDEX.md
