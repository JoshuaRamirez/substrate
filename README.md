# counter-pipeline

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
./bin/dbd /path/to/my.db              # or: COUNTER_DB=/path/to/my.db ./bin/dbd
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
map `/cnt.v5` can already write every value in it, so peers are inside by
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
`counter.h`, no mutex, no blocking, and readers never stall a writer.

This is the same "publish last" shape `dbd` already used for `magic`,
generalised from one word to a buffer.

## The mode switch is one pointer

`counter.h` is the thesis. The two modes differ by which memory a pointer
points at:

```c
alone   ->  c.cell = &c.own    /* private memory in this process */
joined  ->  c.cell = &seg->count   /* a page shared by every peer */
```

`counter_bump()` and `counter_read()` are byte-identical in both modes.
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

## What was cut, and why it was safe

| Cut | Why it isn't required for the claim |
|---|---|
| Arrow / any data format | One `u64`. Format generality is a different axis. |
| IDL + code generation | With one verb, write both impls by hand. Keep the discipline, drop the generator. |
| A ring buffer | An increment is a single atomic. See below. |
| Real engine (LMDB/SQLite/DuckDB) | "Database" here means *single owner of mutable state*. That's the property under test. |
| Data appended to the binary + custom VFS | That is the **read** path. This is a **write**. Orthogonal. |
| Supervisor, manifest, capabilities | Three processes and a hardcoded segment name. |
| Crash recovery, liveness, restart | If `dbd` dies, everyone dies. Acceptable at this size. |
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
| The shared cell needs a lock, and the lock needs a protocol | **not falsified for the cell** — but see below |

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

## What this deliberately does not prove

Request/response latency, payload transfer, backpressure, multi-writer
contention, crash recovery, cross-language ABI. All of those arrive with
**the second verb**.

## Sizes

```
bin/dbd    50K
bin/webd   34K
bin/gui    53K
bin/top    55K
```
