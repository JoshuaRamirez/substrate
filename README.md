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
| `bin/dbd`  | the database. Owns the segment's creation, lifetime, persistence. |
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

## Falsification criteria — results

Run `./test.sh`.

| Criterion | Result |
|---|---|
| `--join` needs different code above the call site | **not falsified** — only the constructor differs |
| Native GUI forces a system-installed toolkit | **not falsified** — Cocoa is the OS; `bin/gui` is 53K |
| The binaries need a shared runtime or launcher to find each other | **not falsified** — one shm name, no discovery |
| The shared cell needs a lock, and the lock needs a protocol | **not falsified at one verb** — watch this one |

The fourth is the one to watch. If a second verb makes it fire, the honest
conclusion is that the ring buffer was load-bearing from the start.

## What this deliberately does not prove

Request/response latency, payload transfer, backpressure, multi-writer
contention, crash recovery, cross-language ABI. All of those arrive with
**the second verb**.

## Sizes

```
bin/dbd    34K
bin/webd   34K
bin/gui    53K
bin/top    54K
```
