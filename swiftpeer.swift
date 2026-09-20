// swiftpeer -- a second ABI joining the substrate.
//
// It shares NOTHING with the C programs: no header, no library, no runtime.
// It knows only the numbers bin/layout prints -- an shm name, a magic word,
// a version, and a table of field offsets. That table is the interface.
//
// It does its own shm_open, its own mmap, and its own atomics. If the C side
// and this side agree, it is because the FORMAT is the contract.

import Darwin

// ---- the contract (see: ./bin/layout) ----
let SHM_NAME   = "/cnt.v7"
let MAGIC:  UInt32 = 0x434E5437
let VERSION: UInt32 = 7
let SEG_SIZE = 67141632

let OFF_MAGIC     = 0
let OFF_VERSION   = 4
let OFF_COUNT     = 8
let OFF_OWNER_PID = 16
let OFF_NEXT_ID   = 336
let OFF_ARENA     = 32768
let BLOCK_SIZE    = 1048576
let OFF_RING      = 7008
let OFF_PEERS     = 8800

let SLOT_SIZE = 56, SLOT_N = 32
let SLOT_STATE = 0, SLOT_OP = 4, SLOT_CLIENT = 8, SLOT_ARG = 16
let SLOT_RESULT = 24, SLOT_LEN = 32, SLOT_BLOCK = 40, SLOT_ERR = 48
let OP_RESERVE: Int32 = 0, OP_GET: Int32 = 2
let SLOT_FREE: UInt32 = 0, SLOT_REQUEST: UInt32 = 1, SLOT_DONE: UInt32 = 2

let PEER_SIZE = 64, PEER_N = 256
let PEER_STATE = 0, PEER_PID = 8, PEER_OPS = 16, PEER_SINCE = 24
let PEER_NAME = 32, PEER_ROLE = 48, NAMELEN = 16

// ---- attach ----
// shm_open is variadic in the C header, so Swift marks it unavailable. Reach
// it through the dynamic linker instead -- still no C shim, still no header.
typealias ShmOpenFn = @convention(c) (UnsafePointer<CChar>, Int32, mode_t) -> Int32
let shmOpen: ShmOpenFn = {
    guard let sym = dlsym(UnsafeMutableRawPointer(bitPattern: -2), "shm_open") else {
        fputs("swiftpeer: libc has no shm_open\n", stderr); exit(4)
    }
    return unsafeBitCast(sym, to: ShmOpenFn.self)
}()

func nowSec() -> Double {
    var t = timespec()
    clock_gettime(CLOCK_MONOTONIC, &t)
    return Double(t.tv_sec) + Double(t.tv_nsec) / 1e9
}

func attach() -> UnsafeMutableRawPointer? {
    let fd = SHM_NAME.withCString { shmOpen($0, O_RDWR, 0o600) }
    if fd < 0 { return nil }
    let p = mmap(nil, SEG_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0)
    close(fd)
    if p == MAP_FAILED { return nil }
    let base = p!
    if base.load(fromByteOffset: OFF_MAGIC, as: UInt32.self) != MAGIC {
        munmap(base, SEG_SIZE); return nil
    }
    if base.load(fromByteOffset: OFF_VERSION, as: UInt32.self) != VERSION {
        munmap(base, SEG_SIZE); return nil
    }
    return base
}

@inline(__always) func u64ptr(_ b: UnsafeMutableRawPointer, _ off: Int)
    -> UnsafeMutablePointer<UInt64> {
    return (b + off).assumingMemoryBound(to: UInt64.self)
}
@inline(__always) func i32ptr(_ b: UnsafeMutableRawPointer, _ off: Int)
    -> UnsafeMutablePointer<Int32> {
    return (b + off).assumingMemoryBound(to: Int32.self)
}

// ---- the same operations the C peers do, on the same bytes ----
// NOTE: OSAtomicAdd64Barrier returns the NEW value; C's atomic_fetch_add
// returns the OLD one. Same memory, same atomicity, opposite convention --
// exactly the kind of seam a cross-ABI contract has to get right, and it cost
// an off-by-one before anyone noticed.
func bump(_ b: UnsafeMutableRawPointer, _ slot: Int) -> UInt64 {
    let p = UnsafeMutableRawPointer(u64ptr(b, OFF_COUNT))
        .assumingMemoryBound(to: Int64.self)
    let v = OSAtomicAdd64Barrier(1, p)
    let o = UnsafeMutableRawPointer(u64ptr(b, OFF_PEERS + slot * PEER_SIZE + PEER_OPS))
        .assumingMemoryBound(to: Int64.self)
    _ = OSAtomicAdd64Barrier(1, o)
    return UInt64(v)
}

func claimSlot(_ b: UnsafeMutableRawPointer, _ name: String, _ role: String) -> Int? {
    for i in 0..<PEER_N {
        let st = i32ptr(b, OFF_PEERS + i * PEER_SIZE + PEER_STATE)
        if OSAtomicCompareAndSwap32Barrier(0, 1, st) {
            let row = b + OFF_PEERS + i * PEER_SIZE
            u64ptr(b, OFF_PEERS + i * PEER_SIZE + PEER_PID).pointee   = UInt64(getpid())
            u64ptr(b, OFF_PEERS + i * PEER_SIZE + PEER_OPS).pointee   = 0
            u64ptr(b, OFF_PEERS + i * PEER_SIZE + PEER_SINCE).pointee = UInt64(time(nil))
            for (off, s) in [(PEER_NAME, name), (PEER_ROLE, role)] {
                let dst = (row + off).assumingMemoryBound(to: CChar.self)
                for k in 0..<NAMELEN { dst[k] = 0 }
                for (k, ch) in Array(s.utf8).enumerated() where k < NAMELEN - 1 {
                    dst[k] = CChar(bitPattern: ch)
                }
            }
            return i
        }
    }
    return nil
}

func releaseSlot(_ b: UnsafeMutableRawPointer, _ i: Int) {
    u64ptr(b, OFF_PEERS + i * PEER_SIZE + PEER_PID).pointee = 0
    let st = i32ptr(b, OFF_PEERS + i * PEER_SIZE + PEER_STATE)
    _ = OSAtomicCompareAndSwap32Barrier(1, 0, st)
}

// The reply-carrying verb, spoken from a different language.
func reserve(_ b: UnsafeMutableRawPointer, _ n: UInt64) -> UInt64 {
    var mine = -1
    for i in 0..<SLOT_N {
        let st = i32ptr(b, OFF_RING + i * SLOT_SIZE + SLOT_STATE)
        if OSAtomicCompareAndSwap32Barrier(Int32(SLOT_FREE), Int32(SLOT_REQUEST), st) {
            mine = i; break
        }
    }
    if mine < 0 { return 0 }
    let s = OFF_RING + mine * SLOT_SIZE
    u64ptr(b, s + SLOT_CLIENT).pointee = UInt64(getpid())
    i32ptr(b, s + SLOT_OP).pointee     = OP_RESERVE
    u64ptr(b, s + SLOT_ARG).pointee    = n
    i32ptr(b, s + SLOT_ERR).pointee    = 0
    OSMemoryBarrier()
    i32ptr(b, s + SLOT_STATE).pointee  = Int32(SLOT_REQUEST)

    let deadline = nowSec() + 2.0
    while nowSec() < deadline {
        OSMemoryBarrier()
        if UInt32(bitPattern: i32ptr(b, s + SLOT_STATE).pointee) == SLOT_DONE {
            let base = i32ptr(b, s + SLOT_ERR).pointee != 0
                     ? 0 : u64ptr(b, s + SLOT_RESULT).pointee
            u64ptr(b, s + SLOT_CLIENT).pointee = 0
            OSMemoryBarrier()
            i32ptr(b, s + SLOT_STATE).pointee = Int32(SLOT_FREE)
            return base
        }
    }
    u64ptr(b, s + SLOT_CLIENT).pointee = 0
    OSMemoryBarrier()
    i32ptr(b, s + SLOT_STATE).pointee = Int32(SLOT_FREE)
    return 0
}

// ---- main ----
guard let base = attach() else {
    fputs("swiftpeer: no owner on \(SHM_NAME)\n", stderr)
    exit(2)
}
guard let slot = claimSlot(base, "swiftpeer", "swift") else {
    fputs("swiftpeer: peer table full\n", stderr)
    exit(3)
}

let args = CommandLine.arguments
var bumps: UInt64 = 1
var want: UInt64 = 0
var i = 1
while i < args.count {
    if args[i] == "--bump", i + 1 < args.count { bumps = UInt64(args[i+1]) ?? 1; i += 1 }
    if args[i] == "--reserve", i + 1 < args.count { want = UInt64(args[i+1]) ?? 0; i += 1 }
    i += 1
}

var last: UInt64 = 0
for _ in 0..<bumps { last = bump(base, slot) }   // already the new value
print("swiftpeer: bumped \(bumps), count=\(last)")
if want > 0 {
    let b = reserve(base, want)
    print("swiftpeer: reserved \(want), base=\(b), last=\(b == 0 ? 0 : b + want - 1)")
}
let owner = u64ptr(base, OFF_OWNER_PID).pointee
print("swiftpeer: owner=\(owner) slot=\(slot) lang=swift")

releaseSlot(base, slot)
munmap(base, SEG_SIZE)
