# substrate

The absolute minimal prototype of a common-process executable pipeline.

## The one claim under test

> An executable is complete alone, and joining a shared pipeline is a flag —
> not a rewrite, not a protocol, and not a network.

## The whole thing

Three single-file executables, no package manager, no dependency tree,
no third-party code. Each is one translation unit.

| | |
|---|---|
| `bin/dbd`  | the database. Owns the segment's creation, lifetime, persistence. Its file can be moved while it runs. |
| `bin/webd` | an HTTP driver. ~110 lines of BSD sockets. |
| `bin/gui`  | a native Cocoa window. Not a webview. No HTML. No toolkit to install. |
| `bin/top`  | a native dashboard of every running peer. Docker Desktop, minus Docker. |
| `bin/look` | one instance, complete alone: a 1,000,000-record database inside the binary. |
| `bin/swiftpeer` | a Swift peer. Different toolchain, different runtime, same page. |
| `pypeer.py` | a Python peer. No compilation at all. |
| `bin/layout` | prints the binary contract the three languages agree on. |
| `bin/bench` | the numbers, against AF_UNIX and TCP loopback. |

They share one `u64` and a 16-slot peer table in one shared-memory page.

## The dashboard

`bin/top` does not run `ps`. It does not poll a daemon. It does not speak a
protocol. Every process writes its own row into the shared peer table on
join, and `top` reads all the rows every frame. Liveness is `kill(pid, 0)` —
the OS answers, so there is no heartbeat to miss. `dbd` reaps rows whose
process is gone, because `dbd` is the owner.

Each row has a `stop` button, which is a `SIGTERM` to that pid.

The registry needs no lock and no protocol for the same reason the count
needs none, but by a different route: **ownership is partitioned.** Each
process is the only writer of its own row. Claiming a free row is one
compare-exchange. That is the entire concurrency story.

## Run it

```sh
./build.sh
./test.sh          # the falsification criteria
./run.sh           # the demo: dbd + webd + gui + top
./run.sh alone     # the standalone half
./run.sh top       # the dashboard by itself
```

Then: `curl localhost:8080/bump` and watch the number change in the native
window. No websocket. No polling endpoint. No serialization. The GUI never
spoke to the web server, and neither of them spoke a protocol.

## Moving the database, live

Where `dbd` persists is not baked into `dbd`. It is **in the shared page**,
so any peer can change it while everything is running:

```sh
./bin/dbd /path/to/my.db              # or: SUBSTRATE_DB=/path/to/my.db ./bin/dbd
curl -X POST -H "X-Admin-Token: $(./bin/top --token)" \\
     "localhost:8080/admin/dbfile?path=/tmp/other.db"     # move it, live
```

In `bin/top`, the database path sits under the header with a `move...`
button next to it. In the web page there is a path field and a `move`
button. Neither one sends a request to `dbd`. They write the path into the
segment and go back to drawing; `dbd` picks it up on its next tick, within
100 ms. **There is no request, no reply, and no one to be down.**

The semantics are *Save As*, not *Open*: the count keeps counting, only its
destination changes. The old file keeps its last value. A path that `dbd`
cannot open is refused — `dbd` proves the file is writable before committing,
and writes the old path back, so the dashboard reverts on its own and never
shows a destination the data is not going to.

Two rules fall out of the segment being shared:

- **Absolute paths only.** Peers have different working directories, so a
  relative path names a different file in each one.
- **The path is seqlocked.** See below — it is the one value here that needs
  a protocol.

Moving it is an admin operation over HTTP. See **The admin layer** below; the
native dashboard needs no token.

## The admin layer

**The trust boundary is the shm file mode, not a password.** Anything that can
map `/sub.v8` can already write every value in it, so peers are inside by
construction — `bin/top` moves the database with no token and no prompt. It
does not ask permission, because asking would be theatre.

What is *outside* is `webd`'s callers. A browser tab can reach port 8080 but
cannot map shared memory. So `dbd` mints 128 random bits into the segment
before it publishes `magic`, and **being able to read that token is the
credential**. `webd` reads it from the segment to check what a caller presents.

```sh
./bin/top --token          # read it out of shm
```

Three properties gate the route, and all three are load-bearing:

| | |
|---|---|
| **POST only** | a `GET` can be fired by `<img src>`; a `POST` cannot |
| **a custom header** | forces a CORS preflight this server never answers, so a cross-origin page cannot even send the request |
| **an unguessable token** | readable only from shm, or from the same-origin page |

The web page gets the token embedded in its HTML. A hostile page may
`fetch('/')`, but the same-origin policy forbids it reading the response — so
the credential reaches your tab and nothing else. The comparison is
constant-time, so a wrong token does not leak its prefix by timing.

This replaces an earlier ungated `GET /dbfile?path=`, which any page on the
internet could have fired at your loopback to overwrite a file of its choosing.

## The exception that proves the rule

Everything else in the segment is either a single atomic (`count`) or a row
whose only writer is the process that claimed it (the peer table). Neither
needs a lock. The path is neither: it is 256 bytes, and any peer may write it.

So it gets the smallest protocol that works — a **seqlock**. Writers take a
sequence counter odd, copy, put it back even. Readers copy, then check the
sequence did not move under them, and retry if it did. About twenty lines in
`substrate.h`, no mutex, no blocking, and readers never stall a writer.

This is the same "publish last" shape `dbd` already used for `magic`,
generalised from one word to a buffer.

## The mode switch is one pointer

`substrate.h` is the thesis. The two modes differ by which memory a pointer
points at:

```c
alone   ->  c.cell = &c.own    /* private memory in this process */
joined  ->  c.cell = &seg->count   /* a page shared by every peer */
```

`sub_bump()` and `sub_read()` are byte-identical in both modes.
There is not even a branch on the hot path. Every call site above them —
the HTTP handler, the mouseDown, the frame loop — is mode-blind. Exactly
one line in each binary knows which mode it is in, and it is the
constructor.

## The inversion

The conventional shape is `browser ←HTTP→ server ←TCP/SQL→ database`:
three protocol boundaries, three serializations, three copies, and a UI
that is a document renderer impersonating an application.

Here, HTTP exists only at the outermost edge, one process thick. Behind it
there is no protocol — there is memory. That demotes the web server from
*being the application* to being **a driver**: an adapter that translates
the outside world onto shared state. The GUI is a peer driver, not a client
of it.

The GUI is immediate-mode: `drawRect:` reads the shared cell every frame.
No cache, no subscription, no invalidation, no push channel. **The frame
loop is the subscription.** That is what the shared-memory model looks like
when you draw it.

## What was cut at the start, and what came back

| Cut | Why it isn't required for the claim |
|---|---|
| Arrow / any data format | One `u64`. Format generality is a different axis. |
| IDL + code generation | With one verb, write both impls by hand. Keep the discipline, drop the generator. |
| A ring buffer | An increment is a single atomic. **Came back** — the reply-carrying verb needed one. |
| Real engine (LMDB/SQLite/DuckDB) | "Database" here means *single owner of mutable state*. That's the property under test. |
| Data appended to the binary + custom VFS | That is the **read** path. **Came back** — see *The database inside the executable*. |
| Supervisor, manifest, capabilities | Three processes and a hardcoded segment name. |
| Crash recovery, liveness, restart | If `dbd` dies, everyone dies. **Came back** — peers detach and re-attach. |
| Async, threads, backpressure | Not reachable at this size. |

The sharpest cut is the ring buffer. **When the boundary is memory, some
operations need no protocol at all** — an increment is a `fetch_add` on a
shared page, and three OS processes agree on it with zero messages
exchanged. You cannot do that over a socket at any price.

Be honest about the consequence: at this scale `dbd` owns the *lifetime and
truth* of the cell, not each individual write. The moment an operation
arrives that cannot be a single atomic, `dbd` becomes a real mediator and
the ring buffer appears. That is the next increment, and it should be
resisted until this one runs.

## The database inside the executable

The other half of the original idea. A 1,000,000-record table is linked into
the binary's read-only `__TEXT` segment with `-sectcreate`, so:

- **dyld maps it at exec.** "Loading the database" is not an operation — no
  `open()`, no `read()`, no deserialise. The records are addressable memory
  when `main()` starts. Carrying 40 MB costs about **245 µs** extra per launch.
- **N copies share one physical copy.** `__TEXT` is read-only and file-backed,
  so the OS page cache deduplicates it with no coordination and no server.
  Measured: 8 instances, each with the whole table resident (39.5 MB RSS
  apiece, **316 MB apparent**), consumed **65 MB** of real memory.
- **The on-disk form is the in-memory form.** Fixed-width records sorted by
  key; lookup is a binary search over mapped bytes, ~1 µs over a million
  records, cache-miss bound. Nothing is ever copied.

```sh
cd /tmp && ~/…/bin/look k000000042     # answers with no owner, no files, no network
```

`webd` carries it too, so one process holds **two** databases: a shared one it
*joins* for writes, and a private read-only one it *carries*.

## Three languages, one page

`bin/layout` prints the contract: shm name, magic, version, every field offset.
That output **is** the interface.

| Peer | Toolchain | How it joins |
|---|---|---|
| `webd`, `gui`, `top` | clang / Objective-C | the `substrate.h` header |
| `bin/swiftpeer` | Swift 6.3 | its own `shm_open` (via `dlsym` — it's variadic), own `mmap`, own atomics. No Foundation, no C shim |
| `pypeer.py` | CPython + ctypes | `shm_open`, `mmap` and the OSAtomic primitives through libSystem. Real atomics, not the GIL |

Neither Swift nor Python shares a header, a library or a build step with the C
programs. Verified: C bumps 2, Swift bumps 3, Python bumps 4 — all four
processes agree the count is 9, and all three take contiguous ranges from one
id space through the ring.

Found only by doing it: `OSAtomicAdd64Barrier` returns the **new** value where
C's `atomic_fetch_add` returns the **old** one. Same memory, same atomicity,
opposite convention. Eight tests now assert the Swift and Python constants
still match what C computes, because two languages agreeing on stale offsets
read garbage and call it agreement.

## The verb that waits

`sub_reserve(n)` is the one call whose answer the caller needs back. Alone
it is an add on private memory; joined it is a ring round trip. **Same call
site.** That was the untested half of the whole claim.

The ring needs no lock either, for a reason worth naming: a slot has exactly
one writer at every point in its cycle — the client owns it in `FREE` and
`DONE`, the owner owns it in `REQUEST`. **Ownership partitioned in time**
rather than in space. The peer table's trick, turned sideways.

## Numbers

`./bin/bench --join`, same machine, same moment:

| | | |
|---|---|---|
| `bump` joined | **2–4 ns** | a store into a shared page |
| `reserve` joined (ring) | **0.5–2 µs** | full cross-process request *and reply* |
| AF_UNIX round trip | 10–15 µs | **7–10× slower** |
| TCP loopback round trip | 50 µs | **20–50× slower** |
| embedded lookup | ~1 µs | binary search over 1,000,000 records |

Absolute numbers drift with machine load; the ratios hold. The first design
promised "single-digit microseconds" and measured nothing. The ring beats that.

Two defects found *by measuring*, both fixed:

- **A ring's latency is its owner's polling interval, nothing else.** `dbd`
  slept 500 µs between polls, which made a 3 ns memory handoff cost **620 µs**
  — worse than the TCP round trip it exists to beat.
- **An iteration count is not a duration.** "200,000 spins ≈ 1 ms" was really
  1.7 *seconds*, and housekeeping fell 17× behind. The same mistake was in the
  client's timeout. Both are measured on a clock now.

## Payloads: bytes that cross without being copied

The ring carries words. A payload goes in the **arena** — 64 blocks of 1 MB in
the segment — and what crosses the boundary is a *descriptor*: block index plus
length. A reader is handed an offset into a page it already has mapped.

`webd` streams a `PUT` body **straight into an arena block** — kernel to shared
page, never landing in a local buffer — then publishes. 900 KB round-trips
byte-identical, and `pypeer.py` reads the same blob out of the arena with a
matching SHA: cross-language zero-copy.

`./bin/blobbench --join`, versus AF_UNIX moving the same bytes:

| payload | reader touches every byte | reader probes 1 B per page |
|---|---|---|
| 4 KB | 4.8× | 6.7× |
| 64 KB | 2.1× | 11.8× |
| 1 MB | **0.9×** | **37.3×** |

**At 1 MB with a full read the arena is slower than the socket.** Both are
memory-bandwidth bound on the read by then, and zero-copy buys nothing if you
were going to touch every byte anyway. That is the honest ceiling, and quoting
the 4 KB number without it would be a lie by selection.

The structural difference is in the sparse column, and it *grows* with size:
**with the arena you pay for what you read; with a socket you pay for what was
sent.**

## Contention, and backpressure that waits instead of losing

`bin/storm` forks N clients onto the 32-slot ring and verifies the ids they
receive form an exact `1..N` cover — no duplicate, no gap. M4 Max, 16 cores:

| clients | throughput | dup | gap | dropped |
|---|---|---|---|---|
| 8 | 1,154 k/s | 0 | 0 | 0 |
| 32 | 1,905 k/s | 0 | 0 | 0 |
| 64 | 383 k/s | 0 | 0 | 0 |
| 256 | 46 k/s | 0 | 0 | 0 |

Throughput peaks at the ring width and degrades gracefully past it. Nothing is
ever lost — that is the difference between a full queue and a failure.

Contention did not just slow this design down, **it found three correctness
bugs**, every one invisible below the core count:

1. **The registry was the limit, not the ring.** The peer table capped at 16,
   so a "64 client" test silently ran 21 and passed.
2. **Spinning starved the owner.** 64 spinning clients on 16 cores took
   throughput from 2.3 M/s to 16 k/s and *dropped* 32 requests. Waiting is now
   spin → `sched_yield` → `usleep`; the yield is the load-bearing step.
3. **Two ways an id could be spent and reach nobody.** A timing-out client
   could reclaim a slot the owner was mid-serve on (3,703 leaked per 128,000),
   and the claim published `REQUEST` before writing its arguments, so the owner
   could read the *previous* caller's (784 leaked). Fixed with a `SERVING`
   state and a `CLAIMED` state — publish the state **last**, the same rule
   `dbd` already used for `magic`.

## Another OS, another libc, another user

`Dockerfile.linux` builds the same sources under Debian, glibc 2.36, Linux
aarch64 — a different kernel, a different libc, a different linker. The
embedded table moves from a Mach-O `__TEXT` section to an ELF
`ld -r -b binary` object; same idea, same page-cache sharing.

| | macOS / Apple libc | Linux / glibc 2.36 |
|---|---|---|
| segment size | 67,141,632 | **67,141,632** |
| magic | `0x434E5437` | `0x434E5437` |
| `look k000000042` | 111486301962 | 111486301962 |
| 64-client storm | 0 dup / 0 gap / 0 drop | 0 dup / 0 gap / 0 drop |
| ring vs AF_UNIX | 7–10× | **36×** |

The struct is byte-identical across two compilers and two libcs, which is the
whole ABI claim in one number.

Feature macros pull in *opposite* directions and the header now knows it:
glibc hides `clock_gettime` under strict `-std=c11` unless you ask for POSIX
2008; Apple's libc **hides the BSD extensions** if you do ask.

**Another user cannot join.** `/dev/shm/sub.v8` is mode `600`, and a second uid
on the same machine gets "no owner" and exits non-zero. The trust boundary is
not a convention — the kernel enforces it.

## Another machine

Not a gap: a *boundary*. Shared memory is single-machine by construction, and
no amount of work here changes that. `webd` already **is** the bridge, and the
cost of crossing it is measured above: TCP loopback is 30–50 µs against the
ring's 0.5–2 µs, so anything remote pays roughly 30× before it leaves the box.

## Hardening

The prototype's early failures were all the same shape: **a shared name is not
a shared lifetime.** `shm_unlink` removes the name, not the mapping, so a peer
holding an unlinked page keeps it alive and never learns the world moved on.
That was a real split brain, observed live, not a hypothesis.

| Was | Now | Test |
|---|---|---|
| Restarting `dbd` silently orphaned every attached peer | Peers check the owner's pid, report `mode=detached`, and re-attach when an owner returns | 11 |
| A second `dbd` took the name and forked the world | Refuses to start while a live owner holds it, and says whose pid | 11 |
| A writer killed mid-seqlock wedged the path forever | The owner repairs the sequence once the writer is provably dead | 12 |
| `persist()` could leave a truncated file reading back as `0` | Temp file → `fflush` → `fsync` → atomic `rename` | 13 |
| One `read()` could split headers and drop the auth header | Reads to the blank line; `431` past 8 KB | — |
| `write()` short-writes could emit a partial header | `writeall()` loops | — |
| `top`'s stop button could signal a recycled pid | Re-checks the slot still holds that pid before `SIGTERM` | — |

Built with `-Wall -Wextra -Wshadow -Wformat=2 -Wformat-security -Wcast-qual
-Wvla -Wwrite-strings -fstack-protector-strong -D_FORTIFY_SOURCE=2`, clean.
`./build.sh --san` builds the same sources under ASan + UBSan; the full suite
passes there, and `webd` survives malformed request lines, header floods,
5 KB tokens, embedded NULs and traversal attempts without a single sanitizer
report.

Two things checked and deliberately **not** changed:

- **`fchmod` on the shm descriptor.** macOS returns `EINVAL`, and it was never
  needed: `umask` can only clear permission bits, never add them, so `0600`
  cannot widen into something group- or world-readable.
- **An admin may write the database anywhere the owner can.** That is what
  admin means. The gate is on *who* asks, not *where* they point it. A path
  the owner cannot open is refused and reverted, so a bad path is inert.

## Falsification criteria — results

Run `./test.sh`.

| Criterion | Result |
|---|---|
| `--join` needs different code above the call site | **not falsified** — only the constructor differs |
| Native GUI forces a system-installed toolkit | **not falsified** — Cocoa is the OS; `bin/gui` is 53K |
| The binaries need a shared runtime or launcher to find each other | **not falsified** — one shm name, no discovery |
| The shared cell needs a lock, and the lock needs a protocol | **not falsified** — see below |
| A reply-carrying verb forces a ring, and the ring breaks the one-pointer property | **not falsified** — the ring exists, `sub_reserve` needs it, and the call site still does not change |
| A second language cannot join without a C shim | **not falsified** — Swift and Python both join on the format alone |

The fourth one fired, and it is worth being exact about how.

A second verb arrived: *move the database*. It did **not** need a lock for
the count, and it did **not** summon the ring buffer. But its payload is a
256-byte path rather than one word, and it has many writers, so it needed a
seqlock — twenty lines, no mutex, no blocking.

So the honest reading is narrower than the criterion was written. It is not
that shared memory needs no protocol. It is that **a protocol is needed per
shape of value, not per system.** A word-sized value with one writer needs
nothing. A buffer with many writers needs publication ordering, and that is
cheap. The ring buffer becomes load-bearing at a third thing: an operation
whose *result* other peers must wait on. Nothing here does yet.

## What this still does not prove

Payload transfer, backpressure, contention at scale, the second OS and the
second user were all on this list. Each is now built, measured and tested, and
three of them found bugs on the way. What is honestly left:

- **Fairness.** No client starves in practice, but nothing *guarantees* it.
  Slot claiming is a scan from index 0, so it is biased, not queued.
- **Mixed-version coexistence.** v7 and v8 flatly refuse each other. Peers
  built at different times cannot share a substrate at all.
- **Capability partitioning.** Anything that can map the segment can write
  every byte of it. The boundary is a file mode, not per-region permissions.
- **More than one machine.** Not a gap — a boundary. See above.

## Sizes

```
bin/dbd    50K
bin/webd   34K
bin/gui    53K
bin/top    55K
```
