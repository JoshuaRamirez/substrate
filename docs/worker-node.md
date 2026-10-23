# A worker node

A Linux machine (a VM, the PC under WSL2) becomes a Substrate worker the Mac
drives: its own registry, its own `sub-net serve` on the private link, its own
GitHub key for the private repos it reads, and trust both ways with the Mac.
Three steps run on the worker, and one command on the Mac runs them all.

```sh
scripts/worker-node/join.sh NAME ADDRESS SSH-KEY-PATH \
    [--user U] [--port 7341] [--self mac] [--deploy-repo OWNER/REPO]...
```

```
 Mac (an SSH client, and this repo's bin/net)            worker (Ubuntu 24.04)
 ───────────────────────────────────────────            ─────────────────────────────
 join.sh ──ssh── install.sh ───────────────────────────▶ tools, clone, build,
                                                          ~/.local/bin/{sub,sub-net,sub-dbd},
                                                          ~/.ssh/substrate-worker-github_ed25519
         ──ssh── the Mac's key.pub ────────────────────▶ ~/.cache/substrate-worker/mac.pub
         ──ssh── identity.sh --trust mac … ─────────────▶ sub-net keygen (once), sub-net trust mac
         ◀─ssh── the worker's key.pub, github.pub ──────
 sub-net trust NAME key.pub --at ADDRESS:PORT
         ──ssh── serve.sh ADDRESS:PORT ─────────────────▶ systemd --user: substrate-dbd,
                                                          substrate-net (sub-net serve --bind),
                                                          linger on
 gh repo deploy-key add github.pub --allow-write=false   (each --deploy-repo)
 sub-net ps NAME  ◀══════ Noise IK over the private link ══════ the proof
```

Every step is **idempotent**. It ends with a line `changed: …`, and run again
on a worker that is already set up it says `changed: nothing` and touches no
file. A second `join.sh` is safe, and is the way to bring a worker up to date
(`SUB_WORKER_REF` picks the commit).

**Only public halves cross the link**: the two `sub-net` key.pubs and the
GitHub key's `.pub`. Scripts and reports name key *paths*, never key values.
Nothing is installed or started on the Mac.

## The steps

**1. `install.sh`** installs the build tools (`apt-get` only for what is
missing), clones `github.com/JoshuaRamirez/substrate` (public, so no key is
needed to read it), builds `bin/sub`, `bin/net` and `bin/dbd`, and copies them
to `~/.local/bin` as `sub`, `sub-net` and `sub-dbd`, only when they differ. It
also makes the worker's own GitHub key pair, `~/.ssh/substrate-worker-github_ed25519`
(ed25519, no passphrase, mode 0600), and points `github.com` at it in
`~/.ssh/config` with one marked block.

**2. `identity.sh`** runs `sub-net keygen` once, then `sub-net trust` on the
driving machine's key.pub, which `join.sh` has already copied over. The Mac is
trusted with no address: it calls in, and the worker never dials it.

**3. `serve.sh ADDRESS:PORT`** writes two systemd user units:
`substrate-dbd` (the registry) and `substrate-net` (`sub-net serve --bind
ADDRESS:PORT`, after the registry). It turns linger on so both start at boot
with no one logged in, and enables and starts them. A unit is rewritten and
restarted only when its text changed. `Restart=always` is also how the adapter
waits for the private link after the nightly shutdown: until the link's
address is up, the bind fails and systemd tries again two seconds later.
`sub-net serve` itself refuses any address that is not loopback or private.

**The join** then fetches the worker's key.pub and GitHub `.pub` into the
Mac's net folder (`workers/NAME/`). It runs `sub-net trust NAME … --at
ADDRESS:PORT`, registers the GitHub key read-only on each `--deploy-repo`
(skipped when a key with the same title is already there), and proves the
join with `sub-net ps NAME`.

## What the hand-off must contain

| Field | Example |
|---|---|
| name | `aspen-worker-1` |
| private address | `10.20.0.4` |
| SSH user | `azureuser` |
| SSH key path (on the Mac) | `~/.ssh/aspen-worker-1_ed25519` |
| repos for deploy keys | `JoshuaRamirez/Desk-Workers` |

The private address is whatever the private link gives the worker: the
address the Mac reaches it on, and the address it serves on. `join.sh` does
not care which link that is.

## The undo

```sh
# on the worker:  stop and disable both services, move the units aside
sh scripts/worker-node/serve.sh --undo
# on the Mac:     stop trusting it
scripts/worker-node/join.sh --undo NAME          # = sub-net untrust NAME
# GitHub:         remove the deploy key titled "substrate-worker NAME"
gh repo deploy-key list -R OWNER/REPO ; gh repo deploy-key delete ID -R OWNER/REPO
```

`serve.sh --undo` is `systemctl --user disable --now substrate-net
substrate-dbd`, and the units are moved aside, not deleted. The worker's keys
and its clone stay where they are.

## The stand-in

`test-worker.sh`, run by `test.sh`, does the whole join against a stand-in on
one machine. The "VM" is a temp HOME with its own segment, its own net folder
and a clean environment, driven through a local executor
(`SUB_WORKER_EXEC`) instead of SSH, the way `test-net.sh` runs two substrates
on 127.0.0.1. A fake `gh` records the deploy-key calls. Then it runs the
same join again and checks that nothing changed, and that no key value
appeared in anything the join printed.

Knobs, for the stand-in and for unusual workers: `SUB_WORKER_EXEC` (an
executor instead of SSH), `SUB_WORKER_REPO`, `SUB_WORKER_REF`,
`SUB_WORKER_NOAPT` (never call apt), and `SUB_WORKER_SERVICE=direct` (no
systemd: write the units, start the two processes directly).
