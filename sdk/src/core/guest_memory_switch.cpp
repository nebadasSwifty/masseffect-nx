/* See guest_memory_switch.h for the design and the reason behind each decision. */

#if defined(__SWITCH__)

#include "guest_memory_switch.h"

#include <malloc.h>
#include <string.h>
#include <cstdio>
#include <cstring>
#include <switch.h>

#include <algorithm>
#include <map>
#include <set>
#include <atomic>
#include <mutex>
#include <vector>

namespace {

constexpr size_t kPageSize  = 0x1000;
/*
 * Large chunks are requested 2 MB aligned, so the kernel can use block descriptors instead of a
 * page table.
 */
constexpr size_t kBlockSize = 0x200000;

constexpr size_t AlignDown(size_t v, size_t a) { return v & ~(a - 1); }
constexpr size_t AlignUp(size_t v, size_t a) { return (v + a - 1) & ~(a - 1); }

/*
 * Large pages (guest_memory_large_pages, see docs/platform-notes.md, "Guest memory page size")
 *
 * The kernel maps with a 2 MB block descriptor only where the virtual address and the physical
 * address are congruent modulo 2 MB (KPageTable::MapContiguous walks up the block sizes while
 * (virt & (size - 1)) == (phys & (size - 1))). The backing of a chunk is memalign'd to 2 MB from
 * the heap, whose 2 MB-aligned virtual addresses sit on 2 MB-aligned physical blocks, so what decides
 * the page size of the window is the window's own alignment. Before this, the window base came from
 * virtmemFindAslr(size, kBlockSize), whose second argument is a guard size, not an alignment: the
 * base was only 4 KB aligned (0x...238000 in a console log) and every view was mapped with 4 KB pages.
 *
 * Modes (init only, set with RexGmConfigure before RexGmInit):
 *   0  as before: window and shadows at whatever address virtmem returns.
 *   1  window base aligned to kWindowAlign and shadows aligned to their chunk alignment. Every view
 *      except 0xE0000000 becomes 2 MB-congruent (that view mirrors mapping offset 0x100001000, so it
 *      is 4 KB out of phase with the others).
 *   2  as 1, and the chunk grid of the physical range (mapping offsets >= 0x100000000) is shifted by
 *      4 KB so that 0xE0000000 is the congruent view, and 0xA0000000, 0xC0000000, 0x7F000000 and the
 *      raw physical view are not. Which of the two pays off depends on which view the game touches.
 */
constexpr size_t kWindowAlign  = 0x2000000;  /* 32 MB: also allows the 32 MB contiguous hint */
constexpr size_t kPhysStart    = 0x100000000ull;
constexpr size_t kE0Phase      = 0x1000;
int    g_large_pages_mode = 0;
size_t g_phys_phase       = 0;

/* Chunk grid: 2 MB granules, shifted by g_phys_phase in the physical range. */
size_t GridDown(size_t off) {
    if (g_phys_phase && off >= kPhysStart) {
        if (off < kPhysStart + g_phys_phase) return kPhysStart;
        return kPhysStart + g_phys_phase +
               AlignDown(off - kPhysStart - g_phys_phase, kBlockSize);
    }
    return AlignDown(off, kBlockSize);
}

size_t GridUp(size_t off) {
    if (g_phys_phase && off > kPhysStart) {
        if (off <= kPhysStart + g_phys_phase) return kPhysStart + g_phys_phase;
        return kPhysStart + g_phys_phase + AlignUp(off - kPhysStart - g_phys_phase, kBlockSize);
    }
    return AlignUp(off, kBlockSize);
}

/* A committed chunk: heap backing, its code alias (the shadow), and the span of the mapping it covers. */
struct Chunk {
    size_t offset;      /* offset in the mapping */
    size_t length;
    void*  backing;     /* from the heap. Inaccessible after mapping it as code */
    void*  shadow;      /* the codeAlias: always RW, a back door */
};

/* A view: a span of the window that mirrors the mapping from `offset`. */
struct View {
    uint8_t* base;
    size_t   offset;
    size_t   length;
};

/* A span already mapped in this operation, so it can be undone if another fails. */
struct Span {
    size_t   win_off;
    uint8_t* src;
    size_t   len;
};

/*
 * The window size, without the lock
 *
 * InGuestWindow() is called on every fault (tens of thousands per second in a race) and only needs
 * to know whether the address falls inside the window. Reading it from State meant taking the global
 * lock, the same one the mappings use, and in the profile the thread that faulted most spent 22.8%
 * of its time in svcArbitrateLock because of this.
 *
 * base and size are set once in RexGmInit and only change again in RexGmShutdown, so an atomic copy
 * is enough and cannot get out of sync.
 */
std::atomic<size_t> g_size_fast{0};

/*
 * The base goes in its own atomic and is published after the size. That way, if a thread sees the
 * base, the size is already set: the other way round (as it used to be) the window could be seen
 * half-initialized, with a valid base and size 0, and then InGuestWindow said no and a legitimate
 * guest fault ended in a fatal stop.
 */
std::atomic<uint8_t*> g_base_fast{nullptr};

struct State {
    /*
     * Exclusive lock, and it cannot be replaced by a reader/writer one.
     *
     * shared_mutex was tried and it hung the game. The idea was sound on paper: the three calls of
     * the fault path only read, so they could run concurrently. But the performance profiler suspends
     * threads to sample them (svcSetThreadActivity in switch_perf.cpp) and then takes this same lock
     * to read the memory counters.
     *
     * With shared_mutex that is fatal: libstdc++ gives writers preference, so if a thread holding the
     * shared lock is suspended, the first writer to arrive queues up, and all following readers queue
     * behind it. The whole process stops. It hung exactly like that in a test: all 17 threads idle and
     * the profiler unable to write a single report.
     *
     * What did pay off from that attempt is in exception_handler_switch.cpp: asking once per fault
     * instead of five times. That reduces lock traffic without changing how it blocks.
     */
    std::mutex mutex;
    uint8_t* base = nullptr;
    size_t   size = 0;
    VirtmemReservation* reservation = nullptr;

    std::vector<View> views;
    /* chunks sorted by offset: allows finding the one that covers an offset */
    std::map<size_t, Chunk> chunks;
    /*
     * Logical permissions per page of the window, keyed by offset from base.
     * Only pages that are not REX_GM_WRITE are stored; REX_GM_WRITE is the common
     * case, so the table stays small. A page with an entry here is unmapped from
     * the window, whether it is committed or not.
     */
    std::map<size_t, RexGmAccess> protection;
    /*
     * Which (view index, chunk offset) pairs are mapped. A chunk is not mapped into
     * all five views when it is committed: it enters each view the first time it is
     * accessed there. The real ceiling on Horizon is the process limit on mappable
     * memory, and mapping too much exhausts it.
     */
    std::set<std::pair<size_t, size_t>> view_mapped;

    size_t   committed = 0;
    size_t   mapped = 0;
    uint32_t last_result = 0;
};

State& S() {
    static State s;
    return s;
}

/*
 * Our own process handle for the svc*Process* calls. Under hbloader (NRO) libnx has the real one hbloader passes.
 * Started as the NSO of an installed NSP (docs/full-nsp.md) libnx has none (INVALID_HANDLE), so a real handle is made
 * the way nx-hbloader makes its own (getOwnProcessHandle): the current-process pseudo-handle is sent to ourselves as
 * a copy handle over a private session, and the kernel hands the receiver a real handle to this process. If that
 * fails, the pseudo-handle itself is used.
 */
struct OwnHandleExchange {
    Handle server = INVALID_HANDLE;
    Handle received = INVALID_HANDLE;
};

void ReceiveOwnProcessHandle(void* arg) {
    auto* x = static_cast<OwnHandleExchange*>(arg);
    void* base = armGetTls();
    hipcMakeRequest(base, HipcMetadata{});
    s32 index = 0;
    if (R_SUCCEEDED(svcReplyAndReceive(&index, &x->server, 1, INVALID_HANDLE, UINT64_MAX))) {
        const HipcParsedRequest r = hipcParseRequest(base);
        if (r.meta.num_copy_handles == 1) x->received = r.data.copy_handles[0];
    }
    // Closing the session without a reply releases the sender (its request then fails, as in hbloader).
    svcCloseHandle(x->server);
}

Handle AcquireOwnProcessHandle() {
    OwnHandleExchange x;
    Handle client = INVALID_HANDLE;
    if (R_FAILED(svcCreateSession(&x.server, &client, 0, 0))) return CUR_PROCESS_HANDLE;
    Thread t;
    if (R_SUCCEEDED(threadCreate(&t, ReceiveOwnProcessHandle, &x, nullptr, 0x4000, 0x20, -2))) {
        if (R_SUCCEEDED(threadStart(&t))) {
            HipcMetadata meta{};
            meta.num_copy_handles = 1;
            hipcMakeRequest(armGetTls(), meta).copy_handles[0] = CUR_PROCESS_HANDLE;
            svcSendSyncRequest(client);
            threadWaitForExit(&t);
        } else {
            svcCloseHandle(x.server);
        }
        threadClose(&t);
    } else {
        svcCloseHandle(x.server);
    }
    svcCloseHandle(client);
    return x.received != INVALID_HANDLE ? x.received : CUR_PROCESS_HANDLE;
}

Handle Proc() {
    static const Handle own = [] {
        const Handle hbl = envGetOwnProcessHandle();
        return hbl != INVALID_HANDLE ? hbl : AcquireOwnProcessHandle();
    }();
    return own;
}

// Made at load time, in an ordinary context: the first Proc() call may otherwise come from the fault handler.
[[maybe_unused]] const Handle g_own_process_at_startup = Proc();

bool Intersect(size_t a, size_t al, size_t b, size_t bl, size_t* lo, size_t* hi) {
    const size_t l = a > b ? a : b;
    const size_t ea = a + al, eb = b + bl;
    const size_t h = ea < eb ? ea : eb;
    if (l >= h) return false;
    *lo = l;
    *hi = h;
    return true;
}

/* Finds the chunk that contains an offset of the mapping. */
Chunk* ChunkAt(State& s, size_t offset) {
    auto it = s.chunks.upper_bound(offset);
    if (it == s.chunks.begin()) return nullptr;
    --it;
    Chunk& c = it->second;
    return (offset >= c.offset && offset < c.offset + c.length) ? &c : nullptr;
}

/* The view that contains a window address. */
const View* ViewFor(State& s, uint64_t addr) {
    for (const View& v : s.views) {
        const uint64_t lo = reinterpret_cast<uint64_t>(v.base);
        if (addr >= lo && addr < lo + v.length) return &v;
    }
    return nullptr;
}

RexGmAccess AccessOf(State& s, size_t page_off) {
    auto it = s.protection.find(page_off);
    return it == s.protection.end() ? REX_GM_WRITE : it->second;
}

/* Is there committed backing behind this window address? */
bool IsCommittedWindowAddress(State& s, uint64_t addr) {
    const View* v = ViewFor(s, addr);
    if (!v) return false;
    const size_t off = v->offset + static_cast<size_t>(addr - reinterpret_cast<uint64_t>(v->base));
    return ChunkAt(s, off) != nullptr;
}

/*
 * Unmaps [win_off, win_off+len) from the window, skipping pages with logical
 * protection, which are already unmapped. `src` is the shadow address that
 * corresponds to win_off.
 *
 * Walk: `it` always points to the first protected page >= cur. Keys are
 * page-aligned and increasing, so skipping a protected page and advancing the
 * iterator keeps that invariant, and the loop always makes progress.
 */
/*
 * Read-only page: try it, and if it is refused, go back to unmapping
 *
 * Why. Watching writes by unmapping the page has an expensive side effect: the
 * guest cannot read from it either, and every read is an exception. Measured in
 * a race: 108,000 emulated reads per second at about 9 us each, which is
 * practically the whole core of the main thread.
 *
 * With the page read-only, watching works the same (the write faults and the
 * handler services it) but reads are free.
 *
 * Why it is not a given. svcSetProcessMemoryPermission only accepts memory whose
 * state allows permission changes. The shadow is mapped with
 * svcMapProcessCodeMemory, which does allow it (it is already used there, with
 * Perm_Rw). The views are mapped with svcMapProcessMemory, and whether Horizon
 * allows it there too is not documented anywhere.
 *
 * So it is tried once. The first page that needs watching is tried with
 * permissions; if the kernel refuses, that is recorded and from then on pages
 * are unmapped as before. Nothing is left half done: if the call fails, it has
 * not changed anything.
 */
enum class ReadOnly { kNoTry, kSi, kNo };
ReadOnly g_read_only = ReadOnly::kNoTry;

/* Returns true if the page was protected through permissions. */
bool ProtectWithPermissions(uint8_t* page, bool read_only) {
    if (g_read_only == ReadOnly::kNo) return false;

    const Result rc = svcSetProcessMemoryPermission(
        Proc(), reinterpret_cast<u64>(page), kPageSize, read_only ? Perm_R : Perm_Rw);
    if (R_FAILED(rc)) {
        if (g_read_only == ReadOnly::kNoTry) {
            g_read_only = ReadOnly::kNo;  // do not try again
        }
        return false;
    }
    g_read_only = ReadOnly::kSi;
    return true;
}
void UnmapWindowRange(State& s, size_t win_off, uint8_t* src, size_t len) {
    if (g_read_only == ReadOnly::kSi) {
        /* No holes here: everything was mapped, watched or not. */
        if (R_SUCCEEDED(svcUnmapProcessMemory(s.base + win_off, Proc(),
                                              reinterpret_cast<u64>(src), len))) {
            s.mapped -= len;
        }
        return;
    }
    const size_t end = win_off + len;
    size_t cur = win_off;
    auto it = s.protection.lower_bound(cur);
    while (cur < end) {
        if (it != s.protection.end() && it->first == cur) {
            cur += kPageSize;
            ++it;
            continue;
        }
        const size_t stop = (it != s.protection.end() && it->first < end) ? it->first : end;
        if (R_SUCCEEDED(svcUnmapProcessMemory(s.base + cur, Proc(),
                                              reinterpret_cast<u64>(src + (cur - win_off)),
                                              stop - cur))) {
            s.mapped -= stop - cur;
        }
        cur = stop;
    }
}

/*
 * When watched pages are read-only instead of unmapped, the whole range is mapped
 * at once and the watched pages then get their permission lowered. Simpler and
 * faster than skipping them.
 */
bool MapWindowRangeReadOnly(State& s, size_t win_off, uint8_t* src, size_t len) {
    const Result rc =
        svcMapProcessMemory(s.base + win_off, Proc(), reinterpret_cast<u64>(src), len);
    if (R_FAILED(rc)) {
        s.last_result = rc;
        return false;
    }
    s.mapped += len;
    for (auto it = s.protection.lower_bound(win_off);
         it != s.protection.end() && it->first < win_off + len; ++it) {
        ProtectWithPermissions(s.base + it->first, true);
    }
    return true;
}

/*
 * Maps [win_off, win_off+len) honoring the logical protection. If a call fails,
 * it undoes what this call had mapped and returns false: it never leaves a range
 * half mapped.
 */
bool MapWindowRange(State& s, size_t win_off, uint8_t* src, size_t len) {
    if (g_read_only == ReadOnly::kSi) {
        return MapWindowRangeReadOnly(s, win_off, src, len);
    }
    const size_t end = win_off + len;
    size_t cur = win_off;
    auto it = s.protection.lower_bound(cur);
    while (cur < end) {
        if (it != s.protection.end() && it->first == cur) {
            cur += kPageSize;
            ++it;
            continue;
        }
        const size_t stop = (it != s.protection.end() && it->first < end) ? it->first : end;
        const Result rc = svcMapProcessMemory(s.base + cur, Proc(),
                                              reinterpret_cast<u64>(src + (cur - win_off)),
                                              stop - cur);
        if (R_FAILED(rc)) {
            s.last_result = rc;
            /*
             * The unmap walks the same way as this loop, so it removes exactly what was
             * mapped, no more and no less.
             */
            UnmapWindowRange(s, win_off, src, cur - win_off);
            return false;
        }
        s.mapped += stop - cur;
        cur = stop;
    }
    return true;
}

/*
 * Maps the range [offset, offset+length) of a chunk into every view that covers
 * it. This is where the 360 mirrors come from.
 *
 * All or nothing: if it fails in one view, it undoes the previous ones. The
 * alternative (leaving a view unmapped and carrying on) would make every access
 * to it be emulated one by one by the exception handler: it would work, but at a
 * fraction of the speed and without any warning. That is exactly the bug the
 * first version had.
 */
bool ViewIndexFor(State& s, uint64_t addr, size_t* out) {
    for (size_t i = 0; i < s.views.size(); i++) {
        const uint64_t lo = reinterpret_cast<uint64_t>(s.views[i].base);
        if (addr >= lo && addr < lo + s.views[i].length) { *out = i; return true; }
    }
    return false;
}

/* Maps a chunk into one view: the view of the faulting access. */
bool MapChunkIntoView(State& s, size_t view_index, const Chunk& c) {
    const View& v = s.views[view_index];
    size_t lo, hi;
    if (!Intersect(c.offset, c.length, v.offset, v.length, &lo, &hi)) return false;
    const size_t win_off = static_cast<size_t>(v.base - s.base) + (lo - v.offset);
    uint8_t* src = static_cast<uint8_t*>(c.shadow) + (lo - c.offset);
    if (!MapWindowRange(s, win_off, src, hi - lo)) return false;
    s.view_mapped.insert({view_index, c.offset});
    return true;
}

void UnmapChunkFromViews(State& s, const Chunk& c) {
    for (size_t i = 0; i < s.views.size(); i++) {
        auto it = s.view_mapped.find({i, c.offset});
        if (it == s.view_mapped.end()) continue;  /* never entered this view */
        const View& v = s.views[i];
        size_t lo, hi;
        if (!Intersect(c.offset, c.length, v.offset, v.length, &lo, &hi)) continue;
        const size_t win_off = static_cast<size_t>(v.base - s.base) + (lo - v.offset);
        uint8_t* src = static_cast<uint8_t*>(c.shadow) + (lo - c.offset);
        UnmapWindowRange(s, win_off, src, hi - lo);
        s.view_mapped.erase(it);
    }
}

void ReleaseChunk(const Chunk& c) {
    svcUnmapProcessCodeMemory(Proc(), reinterpret_cast<u64>(c.shadow),
                              reinterpret_cast<u64>(c.backing), c.length);
    free(c.backing);
}

}  // namespace

extern "C" {

uint8_t* RexGmInit(size_t size) {
    State& s = S();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.base) return s.base;

    virtmemLock();
    if (g_large_pages_mode != 0) {
        /*
         * virtmemFindAslr only page-aligns. Ask for a slice kWindowAlign larger and reserve the
         * aligned part of it; the slack in front stays free address space.
         */
        uint8_t* slice = static_cast<uint8_t*>(virtmemFindAslr(size + kWindowAlign, kBlockSize));
        s.base = slice ? reinterpret_cast<uint8_t*>(
                             AlignUp(reinterpret_cast<size_t>(slice), kWindowAlign))
                       : nullptr;
    } else {
        s.base = static_cast<uint8_t*>(virtmemFindAslr(size, kBlockSize));
    }
    if (s.base) s.reservation = virtmemAddReservation(s.base, size);
    virtmemUnlock();

    if (!s.base || !s.reservation) {
        s.base = nullptr;
        return nullptr;
    }
    s.size = size;
    g_size_fast.store(size, std::memory_order_release);
    g_base_fast.store(s.base, std::memory_order_release);
    return s.base;
}

void RexGmShutdown(void) {
    State& s = S();
    std::lock_guard<std::mutex> lock(s.mutex);

    for (auto& kv : s.chunks) {
        UnmapChunkFromViews(s, kv.second);
        ReleaseChunk(kv.second);
    }
    s.chunks.clear();
    s.views.clear();
    s.protection.clear();
    s.committed = 0;
    s.mapped = 0;

    if (s.reservation) {
        virtmemLock();
        virtmemRemoveReservation(s.reservation);
        virtmemUnlock();
        s.reservation = nullptr;
    }
    s.base = nullptr;
    s.size = 0;
    /*
     * Reverse order from startup: the base is cleared first, so nobody gets in with
     * a size that is no longer valid.
     */
    g_base_fast.store(nullptr, std::memory_order_release);
    g_size_fast.store(0, std::memory_order_release);
}

uint8_t* RexGmBase(void) { return g_base_fast.load(std::memory_order_acquire); }

size_t RexGmSize(void) { return g_size_fast.load(std::memory_order_acquire); }

bool RexGmWindowToOffset(uint64_t window_address, size_t* offset_out) {
    State& s = S();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.base) return false;
    const View* v = ViewFor(s, window_address);
    if (!v) return false;
    if (offset_out) {
        *offset_out =
            v->offset + static_cast<size_t>(window_address - reinterpret_cast<uint64_t>(v->base));
    }
    return true;
}

size_t RexGmCommittedBytes(void) {
    State& s = S();
    std::lock_guard<std::mutex> lock(s.mutex);
    return s.committed;
}

size_t RexGmChunkCount(void) {
    State& s = S();
    std::lock_guard<std::mutex> lock(s.mutex);
    return s.chunks.size();
}

size_t RexGmMappedBytes(void) {
    State& s = S();
    std::lock_guard<std::mutex> lock(s.mutex);
    return s.mapped;
}

int RexGmProtectionMode(void) {
    switch (g_read_only) {
        case ReadOnly::kSi: return 1;
        case ReadOnly::kNo: return 2;
        default: return 0;
    }
}

uint32_t RexGmLastResult(void) {
    State& s = S();
    std::lock_guard<std::mutex> lock(s.mutex);
    return s.last_result;
}

bool RexGmAddView(uint8_t* base, size_t offset, size_t length) {
    State& s = S();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.base) return false;
    if (base < s.base || static_cast<size_t>(base - s.base) + length > s.size)
        return false;  /* outside the reserved window */

    const View v{base, offset, length};

    /*
     * Nothing is mapped here: what is already committed enters this view the first
     * time it is touched.
     */
    s.views.push_back(v);
    return true;
}

bool RexGmCommit(size_t offset, size_t length, RexGmAccess access) {
    /*
     * Memory is always committed accessible. Restricting permissions is the job of
     * RexGmProtect, which works on window addresses and not on mapping offsets (and
     * it has to, because the same offset can be mirrored in several windows with
     * different permissions: the 360 does exactly that).
     *
     * `access` stays in the signature because the natural caller is
     * AllocFixed(kCommit, access), and removing it would invite forgetting to apply
     * it. Pages that already had logical protection keep it.
     */
    (void)access;
    if (length == 0) return true;

    State& s = S();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.base) return false;

    /*
     * Committed in whole kBlockSize granules, not just the requested pages. Guest
     * heaps with 64 KB pages grow one page at a time, and a chunk per request (each
     * a code alias plus one mapping per view that touches it) exhausts the kernel's
     * memory blocks long before the mapping limit: measured, 0xCE01 (2001-0103) on a
     * 64 KB commit with only 329 MB of backing. The extra pages are backing only;
     * guest-level commit state is still kept by the heaps in xmemory.cpp.
     */
    const size_t begin = GridDown(offset);
    const size_t end   = std::min(GridUp(offset + length), s.size);

    size_t cur = begin;
    while (cur < end) {
        if (Chunk* existing = ChunkAt(s, cur)) {
            cur = existing->offset + existing->length;
            continue;
        }

        /* Where the gap ends: the next chunk, or the end. */
        size_t gap_end = end;
        auto sig = s.chunks.upper_bound(cur);
        if (sig != s.chunks.end() && sig->second.offset < gap_end)
            gap_end = sig->second.offset;

        const size_t sz = gap_end - cur;
        const size_t align = (sz >= kBlockSize && GridDown(cur) == cur) ? kBlockSize : kPageSize;

        void* backing = memalign(align, sz);
        if (!backing) {
            s.last_result = REX_GM_ERR_NOMEM;
            return false;
        }
        memset(backing, 0, sz);

        virtmemLock();
        /* The second argument is a guard size; with large pages the alias is aligned by hand. */
        void* shadow = nullptr;
        if (g_large_pages_mode != 0 && align > kPageSize) {
            void* slice = virtmemFindCodeMemory(sz + align, align);
            if (slice) shadow = reinterpret_cast<void*>(AlignUp(reinterpret_cast<size_t>(slice), align));
        } else {
            shadow = virtmemFindCodeMemory(sz, align);
        }
        Result rc = shadow ? svcMapProcessCodeMemory(Proc(), reinterpret_cast<u64>(shadow),
                                                     reinterpret_cast<u64>(backing), sz)
                           : 0;
        virtmemUnlock();

        if (!shadow) {
            free(backing);
            s.last_result = REX_GM_ERR_NOCODEVA;
            return false;
        }
        if (R_FAILED(rc)) {
            free(backing);
            s.last_result = rc;
            return false;
        }

        /* The code alias starts without write access; it has to be opened. */
        rc = svcSetProcessMemoryPermission(Proc(), reinterpret_cast<u64>(shadow), sz, Perm_Rw);
        if (R_FAILED(rc)) {
            svcUnmapProcessCodeMemory(Proc(), reinterpret_cast<u64>(shadow),
                                      reinterpret_cast<u64>(backing), sz);
            free(backing);
            s.last_result = rc;
            return false;
        }

        /*
         * Not mapped into the views: each view gets it on first access
         * (RexGmFaultIn). Mapping it into all five 360 views multiplies by five the
         * use of the process limit on mappable memory.
         */
        const Chunk c{cur, sz, backing, shadow};
        s.chunks[cur] = c;
        s.committed += sz;

        cur = gap_end;
    }
    return true;
}

bool RexGmFaultIn(uint64_t window_address) {
    State& s = S();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.base) return false;

    size_t vi = 0;
    if (!ViewIndexFor(s, window_address, &vi)) return false;

    /*
     * Page with logical protection: it is unmapped on purpose (write watching, MMIO,
     * read-only). Mapping the chunk would not bring it in (protected pages are
     * skipped), so answering yes would leave the fault repeating forever. The next
     * steps of the handler deal with it.
     */
    const size_t win_off =
        static_cast<size_t>(window_address - reinterpret_cast<uint64_t>(s.base));
    if (s.protection.find(AlignDown(win_off, kPageSize)) != s.protection.end()) return false;

    const size_t off = s.views[vi].offset +
                       static_cast<size_t>(window_address -
                                           reinterpret_cast<uint64_t>(s.views[vi].base));
    Chunk* c = ChunkAt(s, off);
    if (!c) return false;                                       /* not committed */
    if (s.view_mapped.count({vi, c->offset}) != 0) return false; /* already there */
    return MapChunkIntoView(s, vi, *c);
}

bool RexGmDecommit(size_t offset, size_t length) {
    State& s = S();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.base) return false;

    const size_t begin = AlignDown(offset, kPageSize);
    const size_t end   = AlignUp(offset + length, kPageSize);

    for (auto it = s.chunks.begin(); it != s.chunks.end();) {
        const Chunk c = it->second;
        /*
         * Only chunks entirely contained in the range are freed: splitting a chunk
         * would also mean splitting its backing, and the backing is a single heap
         * allocation. Partial chunks stay committed, which is conservative but correct.
         */
        if (c.offset >= begin && c.offset + c.length <= end) {
            UnmapChunkFromViews(s, c);
            ReleaseChunk(c);
            s.committed -= c.length;
            it = s.chunks.erase(it);
        } else {
            ++it;
        }
    }
    return true;
}

bool RexGmProtect(uint8_t* address, size_t length, RexGmAccess access, RexGmAccess* out_old) {
    State& s = S();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.base) return false;

    const uint64_t addr  = reinterpret_cast<uint64_t>(address);
    const uint64_t wbase = reinterpret_cast<uint64_t>(s.base);
    if (addr < wbase || addr + length > wbase + s.size) return false;

    const size_t begin = AlignDown(static_cast<size_t>(addr - wbase), kPageSize);
    const size_t end   = AlignUp(static_cast<size_t>(addr - wbase) + length, kPageSize);

    if (out_old) *out_old = AccessOf(s, begin);

    const bool must_be = (access == REX_GM_WRITE);

    /*
     * Fast path, and the common one: asking for full access on a range that has no
     * protected page. AllocFixed(kCommit) does it for every allocation of the game
     * heap, and without this it would walk the range page by page (131,072
     * iterations for the 512 MB of physical memory) to do nothing.
     */
    if (must_be) {
        auto it = s.protection.lower_bound(begin);
        if (it == s.protection.end() || it->first >= end) return true;
    }

    for (size_t p = begin; p < end; p += kPageSize) {
        const bool was = (AccessOf(s, p) == REX_GM_WRITE);

        if (was != must_be) {
            /*
             * The view is looked up per page: a range can cross the boundary
             * between two views with different offsets.
             */
            if (const View* v = ViewFor(s, wbase + p)) {
                const size_t off_mapping =
                    v->offset + (p - static_cast<size_t>(v->base - s.base));
                size_t vi = 0;
                Chunk* c = ChunkAt(s, off_mapping);
                /*
                 * If the chunk has not entered this view yet, there is nothing to map
                 * or unmap: the permission is recorded and the first access will bring
                 * the chunk in with the right permission already.
                 */
                const bool in_view = c != nullptr && ViewIndexFor(s, wbase + p, &vi) &&
                                      s.view_mapped.count({vi, c->offset}) != 0;
                if (in_view) {
                    const u64 src = reinterpret_cast<u64>(
                        static_cast<uint8_t*>(c->shadow) + (off_mapping - c->offset));
                    /*
                     * Permissions are tried first, which keeps the page readable. If the
                     * kernel refuses, it falls back to the usual unmap. ProtectWithPermissions
                     * remembers whether it works.
                     */
                    if (!ProtectWithPermissions(s.base + p, was)) {
                        const Result rc =
                            was ? svcUnmapProcessMemory(s.base + p, Proc(), src, kPageSize)
                                   : svcMapProcessMemory(s.base + p, Proc(), src, kPageSize);
                        if (R_FAILED(rc)) {
                            s.last_result = rc;
                            return false;
                        }
                        if (was) s.mapped -= kPageSize;
                        else s.mapped += kPageSize;
                    }
                }
                /*
                 * Not committed: nothing to map, but the permission is still recorded
                 * and RexGmCommit will honor it on commit.
                 */
            }
        }

        if (must_be) s.protection.erase(p);
        else s.protection[p] = access;
    }
    return true;
}

bool RexGmQueryProtect(uint8_t* address, size_t* length, RexGmAccess* out_access) {
    State& s = S();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.base) return false;

    const uint64_t addr  = reinterpret_cast<uint64_t>(address);
    const uint64_t wbase = reinterpret_cast<uint64_t>(s.base);
    if (addr < wbase || addr >= wbase + s.size) return false;

    const size_t begin = AlignDown(static_cast<size_t>(addr - wbase), kPageSize);

    /*
     * An uncommitted page is reported as having no access, even if its logical
     * permission is the default one.
     *
     * This matters a lot: after a fault, the SDK's MMIO handler queries this, and if
     * it sees the page as accessible it concludes that another thread already
     * unprotected it and retries the instruction. If the page does not really exist,
     * the retry faults again... forever. A silent hang.
     */
    if (!IsCommittedWindowAddress(s, wbase + begin)) {
        if (out_access) *out_access = REX_GM_NONE;
        if (length) *length = kPageSize;
        return true;
    }

    const RexGmAccess a = AccessOf(s, begin);
    if (out_access) *out_access = a;

    /*
     * On input *length is the maximum to walk (0 = one page); on output, the size of
     * the committed range with the same permission.
     */
    size_t max = (length && *length) ? *length : kPageSize;
    if (begin + max > s.size) max = s.size - begin;

    size_t p = begin;
    const size_t cap = begin + max;
    while (p < cap && AccessOf(s, p) == a && IsCommittedWindowAddress(s, wbase + p))
        p += kPageSize;
    if (length) *length = p - begin;
    return true;
}

void* RexGmShadowFor(uint64_t window_address) {
    State& s = S();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.base) return nullptr;

    const View* v = ViewFor(s, window_address);
    if (!v) return nullptr;

    const size_t off_mapping =
        v->offset + static_cast<size_t>(window_address - reinterpret_cast<uint64_t>(v->base));
    Chunk* c = ChunkAt(s, off_mapping);
    if (!c) return nullptr;

    return static_cast<uint8_t*>(c->shadow) + (off_mapping - c->offset);
}

void RexGmConfigure(int large_pages_mode) {
    State& s = S();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.base) return;  /* init only: the window and its chunk grid are fixed once reserved */
    if (large_pages_mode < 0 || large_pages_mode > 2) large_pages_mode = 0;
    g_large_pages_mode = large_pages_mode;
    g_phys_phase = large_pages_mode == 2 ? kE0Phase : 0;
}

int RexGmLargePagesMode(void) { return g_large_pages_mode; }

/*
 * One line describing how the window is mapped. Two independent sources:
 *
 * - svcQueryMemory over the window: the kernel's memory blocks (state/permission runs). It does not
 *   show page-table block sizes, but it shows how many separate mapped runs there are and whether
 *   they start 2 MB aligned.
 * - per mapped 2 MB window granule: whether the window address is congruent modulo 2 MB with the
 *   heap address of its backing ("va"), and, when svcQueryPhysicalAddress (0x54) is allowed, with
 *   its physical address over a physically contiguous 2 MB ("pa"). A granule that passes "pa" and
 *   holds no watched page is mapped with a 2 MB block by the kernel's mapping rule.
 *
 * The lock is held only to copy the bookkeeping; the system calls run without it.
 */
size_t RexGmLayoutSummary(char* buf, size_t cap) {
    if (!buf || cap == 0) return 0;
    buf[0] = '\0';
    const u64 t0 = armGetSystemTick();

    struct Granule {
        uint64_t win;      /* window address, 2 MB aligned */
        uint64_t backing;  /* heap address of its backing */
        uint8_t  view;
        bool     watched;  /* holds a page with logical protection */
    };
    struct ViewInfo {
        uint64_t base;
        size_t   mapped = 0;
    };
    std::vector<Granule> granules;
    std::vector<ViewInfo> views;
    uint64_t wbase = 0;
    size_t wsize = 0, nchunks = 0, shadow_congruent = 0, big_chunks = 0, nprotected = 0;
    {
        State& s = S();
        std::lock_guard<std::mutex> lock(s.mutex);
        if (!s.base) return 0;
        wbase = reinterpret_cast<uint64_t>(s.base);
        wsize = s.size;
        nchunks = s.chunks.size();
        nprotected = s.protection.size();
        for (const View& v : s.views) views.push_back({reinterpret_cast<uint64_t>(v.base), 0});
        for (const auto& kv : s.chunks) {
            const Chunk& c = kv.second;
            if (c.length >= kBlockSize) {
                big_chunks++;
                if (((reinterpret_cast<size_t>(c.shadow) - reinterpret_cast<size_t>(c.backing)) &
                     (kBlockSize - 1)) == 0)
                    shadow_congruent++;
            }
        }
        for (const auto& vm : s.view_mapped) {
            const size_t vi = vm.first;
            const Chunk* c = ChunkAt(s, vm.second);
            if (!c || vi >= s.views.size()) continue;
            const View& v = s.views[vi];
            size_t lo, hi;
            if (!Intersect(c->offset, c->length, v.offset, v.length, &lo, &hi)) continue;
            const uint64_t win_lo = reinterpret_cast<uint64_t>(v.base) + (lo - v.offset);
            const uint64_t win_hi = win_lo + (hi - lo);
            views[vi].mapped += hi - lo;
            for (uint64_t g = AlignUp(win_lo, kBlockSize); g + kBlockSize <= win_hi; g += kBlockSize) {
                const size_t goff = lo + static_cast<size_t>(g - win_lo);
                Granule gr;
                gr.win = g;
                gr.backing = reinterpret_cast<uint64_t>(c->backing) + (goff - c->offset);
                gr.view = static_cast<uint8_t>(vi);
                auto it = s.protection.lower_bound(static_cast<size_t>(g - wbase));
                gr.watched = it != s.protection.end() && it->first < static_cast<size_t>(g - wbase) + kBlockSize;
                granules.push_back(gr);
            }
        }
    }

    /* Kernel memory blocks inside the window. */
    size_t regions = 0, region_bytes = 0, aligned_starts = 0, r_small = 0, r_mid = 0, r_big = 0;
    for (uint64_t a = wbase; a < wbase + wsize;) {
        MemoryInfo mi{};
        u32 pi = 0;
        if (R_FAILED(svcQueryMemory(&mi, &pi, a)) || mi.size == 0) break;
        const uint64_t lo = std::max<uint64_t>(mi.addr, wbase);
        const uint64_t hi = std::min<uint64_t>(mi.addr + mi.size, wbase + wsize);
        if ((mi.type & 0xFF) != MemType_Unmapped && hi > lo) {
            regions++;
            region_bytes += hi - lo;
            if ((lo & (kBlockSize - 1)) == 0) aligned_starts++;
            const uint64_t len = hi - lo;
            if (len < 0x10000) r_small++;
            else if (len < kBlockSize) r_mid++;
            else r_big++;
        }
        a = mi.addr + mi.size;
    }

    /* Per view: granules, va-congruent, pa-eligible. */
    const bool can_pa = envIsSyscallHinted(0x54);
    Result pa_rc = 0;
    std::vector<size_t> g_total(views.size()), g_va(views.size()), g_pa(views.size()),
        g_watched(views.size());
    size_t pa_tested = 0;
    for (const Granule& g : granules) {
        g_total[g.view]++;
        if (g.watched) g_watched[g.view]++;
        if (((g.win - g.backing) & (kBlockSize - 1)) == 0) g_va[g.view]++;
        if (!can_pa || g.watched) continue;
        PhysicalMemoryInfo pmi{};
        const Result rc = svcQueryPhysicalAddress(&pmi, g.win);
        if (R_FAILED(rc)) {
            if (!pa_rc) pa_rc = rc;
            continue;
        }
        pa_tested++;
        const uint64_t phys = pmi.physical_address + (g.win - pmi.virtual_address);
        const bool covers = pmi.virtual_address <= g.win &&
                            pmi.virtual_address + pmi.size >= g.win + kBlockSize;
        if (covers && ((phys - g.win) & (kBlockSize - 1)) == 0) g_pa[g.view]++;
    }

    size_t n = static_cast<size_t>(std::snprintf(
        buf, cap,
        "mode %d, window base mod 2MB 0x%llx | kernel blocks in window: %zu (%zu MB), "
        "<64K %zu, 64K-2M %zu, >=2M %zu, 2MB-aligned starts %zu | chunks %zu, shadows 2MB-congruent "
        "%zu/%zu, watched pages %zu | per view MB, 2MB granules va-congruent/pa-eligible/total "
        "(watched):",
        g_large_pages_mode, (unsigned long long)(wbase & (kBlockSize - 1)), regions,
        region_bytes >> 20, r_small, r_mid, r_big, aligned_starts, nchunks, shadow_congruent,
        big_chunks, nprotected));
    for (size_t i = 0; i < views.size() && n < cap; i++) {
        if (views[i].mapped == 0) continue;
        n += static_cast<size_t>(std::snprintf(
            buf + n, cap - n, " %08llx %zu MB %zu/%s%zu/%zu (%zu)",
            (unsigned long long)(views[i].base - wbase), views[i].mapped >> 20, g_va[i],
            can_pa ? "" : "-", can_pa ? g_pa[i] : 0, g_total[i], g_watched[i]));
    }
    if (n < cap) {
        const u64 us = armTicksToNs(armGetSystemTick() - t0) / 1000;
        if (can_pa) {
            n += static_cast<size_t>(std::snprintf(buf + n, cap - n,
                                                   " | svcQueryPhysicalAddress: %zu granules, first "
                                                   "failure 0x%x | %llu us",
                                                   pa_tested, pa_rc, (unsigned long long)us));
        } else {
            n += static_cast<size_t>(std::snprintf(
                buf + n, cap - n, " | svcQueryPhysicalAddress not allowed (pa column is '-') | %llu us",
                (unsigned long long)us));
        }
    }
    return n < cap ? n : cap - 1;
}

/*
 * TLB micro-benchmark (guest_memory_tlb_benchmark)
 *
 * A random cyclic chain over the 64-byte lines of a buffer: each load gives the offset of the next,
 * so loads are dependent and almost every one misses the caches. The same physical memory is read
 * through mappings that differ only in how the kernel can map them:
 *
 *   heap          the newlib heap itself (mapped by svcSetHeapSize)
 *   shadow        a code alias of it (svcMapProcessCodeMemory), 2 MB aligned
 *   pm+0          svcMapProcessMemory at a 2 MB-congruent address: 2 MB blocks if the physical
 *                 memory allows them (the window with large pages)
 *   pm+64K        congruent only modulo 64 KB: 64 KB contiguous-hint pages at best
 *   pm+4K         congruent only modulo 4 KB: 4 KB pages (the window without large pages)
 *
 * The difference between pm+0 and pm+4K is the TLB cost per cold load. Run twice, over the whole
 * buffer and over its first 8 MB (a working set that fits the A57 L2 TLB reach only with large pages).
 */
namespace {

uint32_t ChaseLoads(const uint8_t* base, uint32_t start, size_t loads) {
    uint32_t off = start;
    for (size_t i = 0; i < loads; i++) off = *reinterpret_cast<const volatile uint32_t*>(base + off);
    return off;
}

void BuildChain(uint8_t* buf, size_t bytes, uint64_t seed) {
    const size_t lines = bytes / 64;
    std::vector<uint32_t> order(lines);
    for (size_t i = 0; i < lines; i++) order[i] = static_cast<uint32_t>(i);
    uint64_t x = seed | 1;
    for (size_t i = lines - 1; i > 0; i--) {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        const size_t j = static_cast<size_t>(x % (i + 1));
        std::swap(order[i], order[j]);
    }
    for (size_t i = 0; i < lines; i++) {
        const uint32_t next = order[(i + 1) % lines];
        *reinterpret_cast<uint32_t*>(buf + size_t(order[i]) * 64) = next * 64u;
    }
}

double MeasureNsPerLoad(const uint8_t* base, size_t loads, uint32_t* sink) {
    *sink += ChaseLoads(base, 0, loads / 4);  /* warm the caches that can be warmed */
    const u64 t0 = armGetSystemTick();
    *sink += ChaseLoads(base, 0, loads);
    const u64 t1 = armGetSystemTick();
    return double(armTicksToNs(t1 - t0)) / double(loads);
}

}  // namespace

void RexGmTlbBenchmark(size_t bytes, size_t loads, RexGmEmitFn emit) {
    if (!emit) return;
    char line[512];
    bytes = AlignUp(std::max<size_t>(bytes, 16 * kBlockSize), kBlockSize);
    if (bytes > 0x40000000) bytes = 0x40000000;
    if (loads < 100000) loads = 100000;

    uint8_t* backing = static_cast<uint8_t*>(memalign(kBlockSize, bytes));
    if (!backing) {
        emit("TLB benchmark: memalign failed");
        return;
    }
    std::memset(backing, 0, bytes);

    const size_t small = std::min<size_t>(bytes, 4 * kBlockSize);
    const size_t sizes[2] = {bytes, small};
    double ns[2][5] = {};
    uint32_t sink = 0;

    /* heap: measured before the alias, which takes the heap pages away. */
    for (int k = 0; k < 2; k++) {
        BuildChain(backing, sizes[k], 0x9E3779B97F4A7C15ull + k);
        ns[k][0] = MeasureNsPerLoad(backing, loads, &sink);
    }

    PhysicalMemoryInfo pmi{};
    Result pa_rc = 0xFFFFFFFF;
    if (envIsSyscallHinted(0x54)) pa_rc = svcQueryPhysicalAddress(&pmi, reinterpret_cast<u64>(backing));

    virtmemLock();
    uint8_t* shadow = nullptr;
    if (void* slice = virtmemFindCodeMemory(bytes + kBlockSize, kBlockSize))
        shadow = reinterpret_cast<uint8_t*>(AlignUp(reinterpret_cast<size_t>(slice), kBlockSize));
    Result rc = shadow ? svcMapProcessCodeMemory(Proc(), reinterpret_cast<u64>(shadow),
                                                 reinterpret_cast<u64>(backing), bytes)
                       : MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
    virtmemUnlock();
    if (R_FAILED(rc)) {
        std::snprintf(line, sizeof(line), "TLB benchmark: code alias failed 0x%x", rc);
        emit(line);
        free(backing);
        return;
    }
    rc = svcSetProcessMemoryPermission(Proc(), reinterpret_cast<u64>(shadow), bytes, Perm_Rw);

    /* Address space for the process-memory mappings: 2 MB aligned plus room for the offsets. */
    virtmemLock();
    uint8_t* dst = nullptr;
    VirtmemReservation* resv = nullptr;
    const size_t span = bytes + 2 * kBlockSize;
    if (void* slice = virtmemFindAslr(span + kBlockSize, kBlockSize)) {
        dst = reinterpret_cast<uint8_t*>(AlignUp(reinterpret_cast<size_t>(slice), kBlockSize));
        resv = virtmemAddReservation(dst, span);
    }
    virtmemUnlock();

    Result map_rc = 0;
    if (R_SUCCEEDED(rc)) {
        const size_t deltas[3] = {0, 0x10000, 0x1000};
        for (int k = 0; k < 2; k++) {
            BuildChain(shadow, sizes[k], 0x9E3779B97F4A7C15ull + k);
            ns[k][1] = MeasureNsPerLoad(shadow, loads, &sink);
            for (int d = 0; d < 3 && resv; d++) {
                uint8_t* at = dst + deltas[d];
                const Result r = svcMapProcessMemory(at, Proc(), reinterpret_cast<u64>(shadow), bytes);
                if (R_FAILED(r)) {
                    if (!map_rc) map_rc = r;
                    continue;
                }
                ns[k][2 + d] = MeasureNsPerLoad(at, loads, &sink);
                svcUnmapProcessMemory(at, Proc(), reinterpret_cast<u64>(shadow), bytes);
            }
        }
    }

    if (resv) {
        virtmemLock();
        virtmemRemoveReservation(resv);
        virtmemUnlock();
    }
    svcUnmapProcessCodeMemory(Proc(), reinterpret_cast<u64>(shadow), reinterpret_cast<u64>(backing),
                              bytes);
    free(backing);

    std::snprintf(line, sizeof(line),
                  "TLB benchmark: %zu loads per run, random 64-byte lines; heap buffer 0x%llx, "
                  "physical 0x%llx (contiguous %llu KB from 0x%llx, query 0x%x), alias permission 0x%x, "
                  "map 0x%x, sink %u",
                  loads, (unsigned long long)reinterpret_cast<uintptr_t>(backing),
                  (unsigned long long)pmi.physical_address, (unsigned long long)(pmi.size >> 10),
                  (unsigned long long)pmi.virtual_address, pa_rc, rc, map_rc, sink);
    emit(line);
    for (int k = 0; k < 2; k++) {
        std::snprintf(line, sizeof(line),
                      "TLB benchmark over %zu MB: ns/load heap %.1f | shadow %.1f | pm+0 (2MB blocks) %.1f "
                      "| pm+64K %.1f | pm+4K (4KB pages) %.1f",
                      sizes[k] >> 20, ns[k][0], ns[k][1], ns[k][2], ns[k][3], ns[k][4]);
        emit(line);
    }
}

}  // extern "C"

#endif /* __SWITCH__ */
