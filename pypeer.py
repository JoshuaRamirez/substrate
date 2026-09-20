#!/usr/bin/env python3
"""pypeer -- a third ABI joining the substrate, with nothing compiled.

Swift proved the format survives a second compiled toolchain. This proves it
survives leaving compilation behind entirely: an interpreted process, started
from source, sharing a page and a protocol with C and Swift binaries.

It shares no header, no library and no build step with them. It knows the same
numbers ./bin/layout prints, and it reaches libc's shm_open, mmap and the
OSAtomic primitives through ctypes. The real atomics matter: without them this
would be a process that *looks* joined and silently corrupts the ring.
"""
import ctypes, os, sys, time

# ---- the contract (see: ./bin/layout) ----
SHM_NAME, MAGIC, VERSION, SEG_SIZE = b"/cnt.v6", 0x434E5436, 6, 2648
OFF_COUNT, OFF_OWNER_PID, OFF_NEXT_ID = 8, 16, 336
OFF_RING, OFF_PEERS = 344, 1624
SLOT_SIZE, SLOT_N = 40, 32
SLOT_STATE, SLOT_CLIENT, SLOT_ARG, SLOT_RESULT, SLOT_ERR = 0, 8, 16, 24, 32
FREE, REQUEST, DONE = 0, 1, 2
PEER_SIZE, PEER_N = 64, 16
PEER_STATE, PEER_PID, PEER_OPS, PEER_SINCE, PEER_NAME, PEER_ROLE = 0, 8, 16, 24, 32, 48
NAMELEN = 16

libc = ctypes.CDLL(None, use_errno=True)

libc.shm_open.argtypes = [ctypes.c_char_p, ctypes.c_int, ctypes.c_uint16]
libc.shm_open.restype = ctypes.c_int
libc.mmap.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int,
                      ctypes.c_int, ctypes.c_int, ctypes.c_long]
libc.mmap.restype = ctypes.c_void_p
libc.munmap.argtypes = [ctypes.c_void_p, ctypes.c_size_t]

# Real atomics, not Python's GIL. These are the same instructions the C and
# Swift peers issue, reached from an interpreter through libSystem.
libc.OSAtomicAdd64Barrier.argtypes = [ctypes.c_int64, ctypes.POINTER(ctypes.c_int64)]
libc.OSAtomicAdd64Barrier.restype = ctypes.c_int64
libc.OSAtomicCompareAndSwap32Barrier.argtypes = [ctypes.c_int32, ctypes.c_int32,
                                                 ctypes.POINTER(ctypes.c_int32)]
libc.OSAtomicCompareAndSwap32Barrier.restype = ctypes.c_bool


class Seg:
    def __init__(self, base):
        self.base = base

    def _i64(self, off):
        return ctypes.cast(self.base + off, ctypes.POINTER(ctypes.c_int64))

    def _i32(self, off):
        return ctypes.cast(self.base + off, ctypes.POINTER(ctypes.c_int32))

    def u32(self, off):
        return self._i32(off).contents.value & 0xFFFFFFFF

    def u64(self, off):
        return self._i64(off).contents.value & 0xFFFFFFFFFFFFFFFF

    def set64(self, off, v):
        self._i64(off).contents.value = ctypes.c_int64(v).value

    def set32(self, off, v):
        self._i32(off).contents.value = ctypes.c_int32(v).value

    def add64(self, off, n):
        """Returns the NEW value, like Swift's OSAtomic -- not C's fetch_add."""
        return libc.OSAtomicAdd64Barrier(n, self._i64(off))

    def cas32(self, off, old, new):
        return bool(libc.OSAtomicCompareAndSwap32Barrier(old, new, self._i32(off)))

    def putstr(self, off, s, n):
        raw = s.encode()[: n - 1].ljust(n, b"\0")
        ctypes.memmove(self.base + off, raw, n)


def attach():
    fd = libc.shm_open(SHM_NAME, os.O_RDWR, 0o600)
    if fd < 0:
        return None
    p = libc.mmap(None, SEG_SIZE, 0x1 | 0x2, 0x0001, fd, 0)   # RW, MAP_SHARED
    os.close(fd)
    if not p or p == 2**64 - 1:
        return None
    s = Seg(p)
    if s.u32(0) != MAGIC or s.u32(4) != VERSION:
        libc.munmap(p, SEG_SIZE)
        return None
    return s


def claim(s, name, role):
    for i in range(PEER_N):
        if s.cas32(OFF_PEERS + i * PEER_SIZE + PEER_STATE, 0, 1):
            b = OFF_PEERS + i * PEER_SIZE
            s.set64(b + PEER_PID, os.getpid())
            s.set64(b + PEER_OPS, 0)
            s.set64(b + PEER_SINCE, int(time.time()))
            s.putstr(b + PEER_NAME, name, NAMELEN)
            s.putstr(b + PEER_ROLE, role, NAMELEN)
            return i
    return None


def release(s, i):
    s.set64(OFF_PEERS + i * PEER_SIZE + PEER_PID, 0)
    s.cas32(OFF_PEERS + i * PEER_SIZE + PEER_STATE, 1, 0)


def bump(s, slot):
    v = s.add64(OFF_COUNT, 1)
    s.add64(OFF_PEERS + slot * PEER_SIZE + PEER_OPS, 1)
    return v


def reserve(s, n, timeout=2.0):
    mine = None
    for i in range(SLOT_N):
        if s.cas32(OFF_RING + i * SLOT_SIZE + SLOT_STATE, FREE, REQUEST):
            mine = i
            break
    if mine is None:
        return 0
    b = OFF_RING + mine * SLOT_SIZE
    s.set64(b + SLOT_CLIENT, os.getpid())
    s.set64(b + SLOT_ARG, n)
    s.set32(b + SLOT_ERR, 0)
    s.set32(b + SLOT_STATE, REQUEST)

    end = time.monotonic() + timeout
    base = 0
    while time.monotonic() < end:
        if s.u32(b + SLOT_STATE) == DONE:
            base = 0 if s.u32(b + SLOT_ERR) else s.u64(b + SLOT_RESULT)
            break
    s.set64(b + SLOT_CLIENT, 0)
    s.set32(b + SLOT_STATE, FREE)
    return base


def main():
    s = attach()
    if s is None:
        print("pypeer: no owner on %s" % SHM_NAME.decode(), file=sys.stderr)
        return 2
    slot = claim(s, "pypeer", "python")
    if slot is None:
        print("pypeer: peer table full", file=sys.stderr)
        return 3

    a = sys.argv
    nb = int(a[a.index("--bump") + 1]) if "--bump" in a else 1
    nr = int(a[a.index("--reserve") + 1]) if "--reserve" in a else 0

    last = 0
    for _ in range(nb):
        last = bump(s, slot)
    print("pypeer: bumped %d, count=%d" % (nb, last))
    if nr:
        base = reserve(s, nr)
        print("pypeer: reserved %d, base=%d, last=%d"
              % (nr, base, 0 if base == 0 else base + nr - 1))
    print("pypeer: owner=%d slot=%d lang=python" % (s.u64(OFF_OWNER_PID), slot))

    release(s, slot)
    libc.munmap(s.base, SEG_SIZE)
    return 0


if __name__ == "__main__":
    sys.exit(main())
