# Features

What substrate does, as of 0.3.0. Each line names where it lives and what
proves it. The README tells the story; this is the inventory.

## The segment (`substrate.h`, format v8)

- **One shared counter, two modes, one call site.** `sub_open(&c, join, …)`;
  alone, `c.cell` points at private memory, joined, at the segment. `sub_bump`
  and `sub_read` are identical in both. (test.sh 1–3)
- **The peer table (the registry).** 256 rows of pid, name, role, since, ops;
  each row has one writer; liveness is `kill(pid, 0)`; the owner reaps dead
  rows. (test.sh)
- **Surviving the owner.** Peers detect a dead or replaced owner, report
  `detached`, and re-join a new one. A second owner refuses to start. (11)
- **The database path**, retargetable live by any peer, under a seqlock the
  owner repairs if a writer dies mid-write. (12)
- **The ring.** 32 request slots for verbs whose answer the caller waits for:
  `sub_reserve` (an id range), `sub_put` / `sub_publish` / `sub_get` (blobs).
  Backpressure waits instead of dropping; no id is handed out twice. (storm)
- **The arena.** 64 blocks of 1 MB, read in place, never copied across the
  boundary.
- **The admin token**, minted by the owner into the segment, demanded by webd.
- **`SUB_SHM`**: a second segment on the same machine (a suite's, beside the
  desk's) that never sees the first's rows.

## The programs

- **`dbd`** (`sub-dbd`): owns the segment, persists the count (fsync and
  rename), reaps rows, serves the ring. `--stop`, `--unlink`.
- **`sub`**: `run`, `ps`, `stop`, `enable` / `disable` / `enabled` (launchd
  agents, timers, login apps), `restart`, `change` / `changes` (the
  system-change lane: one change at a time, in arrival order, logged),
  `net …` and `keygen` (hand off to sub-net), `--version`.
- **`sub-net`** (`bin/net`), new in 0.2.0: below.
- **`webd`** (`sub-webd`): HTTP over the segment, with the embedded
  1,000,000-record table and the admin routes.
- **`look`**, **`layout`**, **`bench`**: the embedded table, the contract,
  the numbers.
- **`gui`**, **`top`** (macOS, Cocoa): a window and the dashboard (sortable
  headers, the sort file, `--press`, `--sort`). **`peer`**: their checks
  without the window, for Linux.
- **`pypeer.py`**, **`swiftpeer`**: two more languages joining on the format
  alone.

## Across machines (`sub-net`, 0.2.0)

- **The seam.** `sub_net_transport` in `substrate.h`: listen, accept, dial,
  send, recv, fd, shutdown, close. A transport carries whole frames, in order,
  between two authenticated machines, and nobody in between reads them.
- **The transport shipped: `Noise_IK_25519_ChaChaPoly_SHA256` over TCP.**
  Each machine is known by an X25519 key. Caller and server prove their keys
  in the same two messages; each direction gets its own key; the ephemeral
  keys die with the handshake (forward secrecy).
- **The cipher suite is implemented in-tree, in `net.c`.** There is no
  libsodium, no OpenSSL and no third-party code. `sub-net selftest` checks it
  against the published vectors, and the suite runs it:
  - SHA-256: FIPS 180-2, `abc` and the two-block message
  - HMAC-SHA-256: RFC 4231 test case 2
  - ChaCha20: RFC 8439 §2.3.2 (the block function)
  - Poly1305: RFC 8439 §2.5.2
  - ChaCha20-Poly1305: RFC 8439 §2.8.2 (ciphertext and tag)
  - X25519: RFC 7748 §5.2, and §6.1 (both public keys, the shared secret from
    each side)
  - and two properties: a sealed frame with one flipped bit is refused, and a
    low-order public key is refused.
- **The listener is refused anything but a loopback or private address.**
  `--bind` names exactly one address: 127/8, 10/8, 172.16/12, 192.168/16,
  100.64/10, 169.254/16, `::1`, `fc00::/7` or `fe80::/10`. `0.0.0.0`, `::` and
  public addresses are refused.
- **Keys and trust**: `sub-net keygen` (the secret half has mode 0600 and is
  never printed), `sub-net trust NAME KEY.PUB --at ADDR:PORT`, `sub-net peers`.
  They live in the net folder (`$SUB_NET_DIR`, else the user's config), never
  in the repo.
- **The registry, read across**: `sub-net ps MACHINE [--json]`.
- **Process control, across**: `sub-net run`, `stop` (through the far side's
  change lane), `enable`, `disable`, each done by that machine's own `sub`.
- **Events, pushed live**: `sub-net events MACHINE`. join, leave, update,
  ops, count and registry, one JSON object a line, each with `seq` (gapless
  per subscriber) and the segment's `count` and `next_id`.
- **Named line channels**: `sub-net listen NAME -- cmd` (cmd's stdio is the
  channel) and `sub-net connect MACHINE NAME`, one JSON value a line. This is
  what Warp Drive cells run through.
- **Proof**: `test-net.sh`, two substrates on one machine on 127.0.0.1, 33
  checks.

## A worker node (`scripts/worker-node/`, 0.3.0)

- **`join.sh NAME ADDRESS SSH-KEY-PATH`**, run on the Mac: the three steps
  below on the worker over SSH, the public halves exchanged both ways,
  `sub-net trust`, read-only GitHub deploy keys (`--deploy-repo`, skipped when
  already there), and the proof, `sub-net ps NAME`. `--undo NAME` untrusts it.
  Nothing is installed or started on the Mac.
- **`install.sh`**: build tools (apt only for what is missing), the repo
  cloned and built at a ref, `sub`, `sub-net` and `sub-dbd` in `~/.local/bin`,
  and the worker's own GitHub ed25519 key with a marked `~/.ssh/config` block.
- **`identity.sh`**: `sub-net keygen` once, and trust of the driving
  machine's key.pub.
- **`serve.sh ADDRESS:PORT`**: `substrate-dbd` and `substrate-net` as systemd
  user units with linger, so they come back after a nightly shutdown;
  `--undo`.
- **Every step is idempotent**: it reports `changed: nothing` and touches no
  file when run on a worker that is already set up.
- **Key paths only**: no script prints a key value.
- **`sub-net untrust NAME`**, **`sub-net dir`**, and a `sub-net trust` that
  leaves an identical entry alone.
- **Proof**: `test-worker.sh`, the whole join against a stand-in "VM" (a temp
  HOME with a clean environment, driven by a local executor instead of SSH),
  run twice.

## Platforms

- macOS (clang, Cocoa, Swift) and Linux (clang, glibc; `Dockerfile.linux`,
  CI). The segment is byte-identical on both.
