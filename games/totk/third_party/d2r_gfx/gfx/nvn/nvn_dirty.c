// 客体内存写保护脏页跟踪：预排布贴图（CPU 写、GPU 读）的池页上传后设只读，游戏一写即缺页 → 标脏、解保护；
// 使用前查覆盖页的写代号，比上传时新就重传。NVN 语义：贴图描述符指向池内存，GPU 每次读的是当前内容
// （实测游戏会把流式素材写进仍被采样的贴图存储，不重新登记）。
// 信号处理约束：缺页路径只用原子操作与 mprotect，不分配、不加锁。
// Write-protect dirty-page tracking for guest memory: pool pages of packaged textures (CPU writes, GPU reads) are set read-only after upload, and as soon as the game writes it faults -> mark dirty, unprotect;
// before use, check the write generation of the covered pages and re-upload if newer than at upload time. NVN semantics: a texture descriptor points to pool memory, and the GPU reads the current content every time
// (measured: the game writes streamed assets into texture storage that is still being sampled, without re-registering).
// Signal handler constraint: the page-fault path uses only atomic operations and mprotect, no allocation, no locks.
#include "nvn_dirty.h"
#include "../../../../runtime/uc_compat.h"
#include "../../host/dm.h"
#include <stdatomic.h>
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>
#include <mach/mach_time.h>
#include <sched.h>

// 页状态与 mprotect 的次序（改保护的一方先把状态改成 BUSY，mprotect 落地后才改成终态；别的线程见 BUSY 就等）：
//   设只读：NONE → BUSY → mprotect(只读) → PROT；解保护：PROT → BUSY → mprotect(可写) → NONE。
// 不变式：状态是 NONE 的页一定可写、是 PROT 的页一定只读。原先设只读是「先记 PROT、攒一段再 mprotect」，这段时间里另一线程的写前解保护
// 会把状态改回 NONE，随后攒批的 mprotect 落地 ⇒ 页只读而状态 NONE：游戏线程写它时缺页处理只回「重执行」永远空转；
// 宿主用系统调用往里读文件时 prep_write 看状态 NONE 跳过，系统调用不走缺页处理，直接 EFAULT、数据读不全。
// BUSY 最长持续 = 一次 mprotect 的时间（设只读时再加上给这一段逐页改状态的几次原子写）。等待有上限 BUSY_WAIT_MS，超限报红：
// 缺页处理里超限按「不是我们的页」返回，交给崩溃处理如实报崩，不静默空转。等待只读原子量、不拿任何锁。
// Order of page state and mprotect (the side changing protection first sets the state to BUSY and sets the final state only after mprotect has landed; other threads wait when they see BUSY):
//   set read-only: NONE -> BUSY -> mprotect(read-only) -> PROT; unprotect: PROT -> BUSY -> mprotect(writable) -> NONE.
// Invariant: a page whose state is NONE is always writable, and one whose state is PROT is always read-only. Setting read-only used to be "record PROT first, batch up a run, then mprotect"; during that time another thread's unprotect-before-write
// could set the state back to NONE, and then the batched mprotect landed => the page read-only with state NONE: when the game thread wrote to it the page-fault handler only returned "re-execute" and spun forever;
// when the host read a file into it with a system call, prep_write saw state NONE and skipped, the system call did not go through the page-fault handler and failed with EFAULT, leaving the data incomplete.
// BUSY lasts at most one mprotect (plus, when setting read-only, the few atomic writes that change the state of the pages in the run). The wait has an upper limit BUSY_WAIT_MS and reports red beyond it:
// in the page-fault handler, exceeding it returns "not our page" and lets the crash handler report the crash truthfully instead of spinning silently. The wait only reads atomics and takes no lock.
enum { PG_NONE = 0, PG_PROT = 1, PG_BUSY = 2 };
enum { BUSY_WAIT_MS = 2000 };
// 跟踪区间 = 各内存池（MemoryPoolInitialize 登记）。池销毁后复用 live=0 的槽，信号处理里无锁扫描。
// Tracked ranges = the memory pools (registered by MemoryPoolInitialize). After a pool is destroyed its live=0 slot is reused; the signal handler scans without locks.
#define MAX_RANGE 16384 // 原为 256：内存池多于它时超出的池不跟踪，里面的贴图每次取用都当成脏的重传（暗黑实测峰值见内存读数行的「内存池」；塞尔达约 300 个，踩过）。扫描只到已用的槽数，容量大不增加开销 | was 256: with more memory pools than that the excess pools are untracked and their textures are treated as dirty and re-uploaded on every use (for Diablo's measured peak see "memory pools" in the memory readout line; Zelda has about 300 and hit this). The scan only covers slots in use, so a larger capacity costs nothing. TOTK (2026-10-17): 4096 was full in the user's own games and most long test games (about 3,900 live pools in the world, more when moving between regions); now 16384
typedef struct { uintptr_t base, end; _Atomic uint8_t *state; _Atomic uint64_t *gen; size_t cap; _Atomic int live; _Atomic uint64_t latest; unsigned shift; } range_t;   // latest (TOTK): highest page generation in the range
/* TOTK: shift = the tracking block of the range ("page" below): the system page, or 64 KiB in a zero-copy pool. Every
   protect / unprotect of part of a mapping splits it; memfd pieces merge back, dma-buf pieces never do, and a 503 MiB
   zero-copy pool tracked per 4 KiB page ran the process into vm.max_map_count within a minute (Room of Awakening):
   mprotect and mmap failed, the game hung or crashed. 64 KiB blocks: at most 1/16 of the pieces. */
static inline uintptr_t blk_addr(const range_t *r, uintptr_t p) { return r->base + (p << r->shift); }
static inline size_t blk_len(const range_t *r, uintptr_t p, uintptr_t n) { uintptr_t a = blk_addr(r, p), e = a + (n << r->shift); return (e < r->end ? e : r->end) - a; }
int mtl_pool_zero_copy(const void *mem) __attribute__((weak));   /* the Vulkan backend: the pool is dma-buf memory */
static inline uintptr_t blk_count(const range_t *r) { return (r->end - r->base + ((uintptr_t)1 << r->shift) - 1) >> r->shift; }   // latest (TOTK): highest page generation in the range
static range_t g_r[MAX_RANGE];
static _Atomic int g_nr;
static uintptr_t g_pg, g_shift;
static _Atomic uint64_t g_now = 1;
static _Atomic uint64_t g_faults, g_preps;
static _Atomic uint64_t g_adds_total;
static _Atomic int g_live, g_live_peak;   // 在册的池数与峰值（内存读数行用） | number of registered pools and peak (for the memory readout line)
void nvn_dirty_stat(int *live, int *peak, int *cap) { *live = atomic_load(&g_live); *peak = atomic_load(&g_live_peak); *cap = MAX_RANGE; }
static _Atomic int g_overflow_logged;
static pthread_mutex_t g_slot_mu = PTHREAD_MUTEX_INITIALIZER;   // 建池 / 销池（客体线程，可能并发）互斥选槽；缺页处理路径不碰它 | Pool creation / destruction (guest threads, possibly concurrent) pick slots under a mutex, and TOTK's protect holds it from its lookup to its last mprotect; the page-fault handling path does not touch it
static _Atomic uint64_t g_mp_calls, g_mp_pages, g_mp_ticks;   // 本模块的 mprotect：调用次数、覆盖页数、耗时（mach 时基，缺页路径里也安全） | This module's mprotect: call count, pages covered, time spent (mach timebase, safe in the page-fault path too)
static int mp(uintptr_t a, size_t len, int prot) {   /* 0, or -1 when mprotect failed (see defrag) */
    uint64_t t = mach_absolute_time(); int r = mprotect((void *)a, len, prot);
    atomic_fetch_add(&g_mp_ticks, mach_absolute_time() - t); atomic_fetch_add(&g_mp_calls, 1); atomic_fetch_add(&g_mp_pages, len >> g_shift);
    return r;
}

static mach_timebase_info_data_t g_tb;   // 首次登记池时取好（缺页路径里只做乘除） | Obtained when a pool is first registered (the fault path only multiplies and divides)
static _Atomic int g_busy_red;
// 等这一页离开 BUSY。返回 1 = 已离开；0 = 等过了上限（报红一次）。信号处理里可用：只有原子读、mach_absolute_time、sched_yield
// Wait for this page to leave BUSY. Returns 1 = it has left; 0 = waited past the limit (reported red once). Usable in a signal handler: only atomic reads, mach_absolute_time and sched_yield
static int busy_wait(range_t *r, uintptr_t p) {
    uint64_t t0 = 0;
    for (unsigned n = 1; atomic_load(&r->state[p]) == PG_BUSY; n++) {
        if (n & 0x3ff) continue;
        uint64_t now = mach_absolute_time(); if (!t0) t0 = now;
        if ((now - t0) * g_tb.numer / g_tb.denom > (uint64_t)BUSY_WAIT_MS * 1000000ull) {
            if (!atomic_exchange(&g_busy_red, 1)) LOG("【红】脏页跟踪：页 %p 停在「忙」超过 %d 毫秒（改保护的线程没走完），不再等", (void *)blk_addr(r, p), BUSY_WAIT_MS);
            return 0; }
        sched_yield();
    }
    return 1;
}
/* TOTK: a sorted index of the live ranges for find(): rebuilt under g_slot_mu when a range is added or removed (normal
   context: find() also runs in the page-fault handler, which must not allocate), published with a pointer swap; an
   index is freed 8 rebuilds later (a reader holds one only for a binary search). The open world has thousands of pools,
   and the linear scan after the recent-hit cache missed was 7% of the translation thread. */
typedef struct range_index { uint64_t gen; int n; uintptr_t *base, *end; int *slot; struct range_index *retired; } range_index;
static _Atomic(range_index *) g_index; static _Atomic uint64_t g_index_gen;   /* g_index_gen: bumped on every add / remove */
/* Replaced indexes are freed only while no find() is inside one (g_index_readers: find() counts itself in before it
   loads g_index, all seq_cst, so a rebuild that sees 0 after publishing knows nobody holds an old index; find() also
   runs in the fault handler, which this keeps lock-free). */
static range_index *g_index_retired; static _Atomic int g_index_readers;
static int range_index_cmp(const void *x, const void *y) { uintptr_t a = g_r[*(const int *)x].base, b = g_r[*(const int *)y].base; return (a > b) - (a < b); }
static void range_index_rebuild(void) {   /* holding g_slot_mu */
    uint64_t gen = atomic_fetch_add(&g_index_gen, 1) + 1; int n = atomic_load(&g_nr), live = 0;
    range_index *x = malloc(sizeof *x); int *slots = malloc(sizeof *slots * (size_t)(n ? n : 1));
    if (!x || !slots) { free(x); free(slots); return; }   /* find() falls back to the scan (the index's gen is old) */
    for (int i = 0; i < n; i++) if (atomic_load_explicit(&g_r[i].live, memory_order_acquire)) slots[live++] = i;
    qsort(slots, (size_t)live, sizeof *slots, range_index_cmp);
    x->gen = gen; x->n = live; x->slot = slots; x->base = malloc(sizeof *x->base * (size_t)(live ? live : 1)); x->end = malloc(sizeof *x->end * (size_t)(live ? live : 1));
    if (!x->base || !x->end) { free(x->base); free(x->end); free(slots); free(x); return; }
    for (int i = 0; i < live; i++) { x->base[i] = g_r[slots[i]].base; x->end[i] = g_r[slots[i]].end; }
    x->retired = NULL; range_index *old = atomic_exchange(&g_index, x);
    if (old) { old->retired = g_index_retired; g_index_retired = old; }
    if (!atomic_load(&g_index_readers))
        while (g_index_retired) { range_index *d = g_index_retired; g_index_retired = d->retired; free(d->base); free(d->end); free(d->slot); free(d); }
}
static range_t *find(uintptr_t a) {
    /* TOTK: the replay asks about the same few pools over and over; the last hits (8, round robin) are checked
       first (a slot reused for another pool fails the bounds or live test and falls through to the scan), so the scan over
       the ~4,000 live pools of the world mostly runs for the first touch of a pool. */
    /* The caches are shared by all threads, not thread-local: every entry is validated (live, contains a) before use, so a
       race between threads only costs a miss, and a __thread variable in this dlopen'ed library goes through the TLS
       descriptor resolver on every access (~23% of nvn_dirty_since on the replay thread). */
    /* The pool last found for this 64 KiB block (direct-mapped) first: pools do not overlap, so a live pool that
       contains the address is the answer; a stale entry fails the test. The binary search below was ~4% of the replay
       thread. Each candidate's bounds are tested with plain loads first and its live flag (an acquire, which keeps
       later loads behind it) only when they contain the address, then the bounds again behind it: acquiring every
       recent entry's flag in turn serialized their cache misses (~4% of the replay thread, 2026-10-24). */
    static range_t *_Atomic block[4096]; unsigned bk = (unsigned)(a >> 16) & 4095;
#define FIND_HIT(l) ((l) && a >= (l)->base && a < (l)->end && atomic_load_explicit(&(l)->live, memory_order_acquire) && a >= (l)->base && a < (l)->end)
    { range_t *l = atomic_load_explicit(&block[bk], memory_order_relaxed); if (FIND_HIT(l)) return l; }
    static range_t *_Atomic recent[8]; static _Atomic unsigned next;
    for (int k = 0; k < 8; k++) {
        range_t *l = atomic_load_explicit(&recent[k], memory_order_relaxed);
        if (FIND_HIT(l)) { atomic_store_explicit(&block[bk], l, memory_order_relaxed); return l; }
    }
#undef FIND_HIT
    atomic_fetch_add(&g_index_readers, 1);
    range_index *x = atomic_load(&g_index);
    if (x && x->gen == atomic_load_explicit(&g_index_gen, memory_order_acquire)) {   /* current: its answer is the answer */
        int lo = 0, hi = x->n - 1, at = -1;
        while (lo <= hi) { int mid = (lo + hi) >> 1; if (x->base[mid] <= a) { at = mid; lo = mid + 1; } else hi = mid - 1; }
        if (at >= 0 && a < x->end[at]) { range_t *r = &g_r[x->slot[at]];
            if (atomic_load_explicit(&r->live, memory_order_acquire) && a >= r->base && a < r->end) { atomic_fetch_sub(&g_index_readers, 1);
                atomic_store_explicit(&recent[atomic_fetch_add_explicit(&next, 1, memory_order_relaxed) & 7], r, memory_order_relaxed);
                atomic_store_explicit(&block[bk], r, memory_order_relaxed); return r; } }
        if (x->gen == atomic_load_explicit(&g_index_gen, memory_order_acquire)) { atomic_fetch_sub(&g_index_readers, 1); return NULL; }
    }
    atomic_fetch_sub(&g_index_readers, 1);
    int n = atomic_load(&g_nr);
    for (int i = 0; i < n; i++) if (atomic_load_explicit(&g_r[i].live, memory_order_acquire) && a >= g_r[i].base && a < g_r[i].end) { atomic_store_explicit(&recent[atomic_fetch_add_explicit(&next, 1, memory_order_relaxed) & 7], &g_r[i], memory_order_relaxed); return &g_r[i]; }
    return NULL;
}
/* TOTK diagnostic (on with DM_NVN_TILELOG): a ring of the latest page unprotections with the writer, read by
   nvn_dirty_recent: game writes (fault: pc / lr of the faulting code) and host writes (dm_guest_write_prepare caller). */
#include <sys/ucontext.h>
static nvn_dirty_rec g_drec[4096]; static _Atomic uint64_t g_drec_n; static int g_drec_on;
uint64_t dm_host_write_pc;   // set by dm_guest_write_prepare (runtime/gfx_bridge.c) before calling prep_write
static void drec_add(uintptr_t page, uint64_t pc, uint64_t lr, uint32_t src) {
    if (!g_drec_on) return;
    uint64_t i = atomic_fetch_add(&g_drec_n, 1) % 4096; g_drec[i] = (nvn_dirty_rec){ page, pc, lr, src };
}
int nvn_dirty_recent(uintptr_t lo, uintptr_t hi, nvn_dirty_rec *out, int max) {
    uint64_t n = atomic_load(&g_drec_n); int k = 0;
    for (uint64_t j = n > 4096 ? n - 4096 : 0; j < n && k < max; j++) { nvn_dirty_rec r = g_drec[j % 4096]; if (r.page >= lo && r.page < hi) out[k++] = r; }
    return k;
}
/* TOTK: mprotect failed: almost surely vm.max_map_count. Every page protect / unprotect splits the pool's mapping, and
   in a zero-copy pool (dma-buf, compat_guest_overlay) the pieces never merge again; a cutscene video streaming into one
   reached the limit, after which an unprotect failed, the page stayed read-only while marked writable, and the game
   thread faulted on it forever. Take every page of the pool (waiting out the busy ones), map the pool again as one
   read-write mapping, and mark every page written (one round of re-uploads). Skip: a page the caller holds busy. */
int compat_guest_remap(void *address, size_t size) __attribute__((weak));
static pthread_mutex_t g_defrag_mu = PTHREAD_MUTEX_INITIALIZER;
static void defrag(range_t *r, uintptr_t skip_lo, uintptr_t skip_hi) {
    if (pthread_mutex_trylock(&g_defrag_mu)) { sched_yield(); return; }   // another thread is at it; the caller's fault retries
    static _Atomic unsigned count; unsigned k = atomic_fetch_add(&count, 1) + 1;
    uintptr_t np = blk_count(r);
    uint8_t *took = calloc(np, 1);
    for (uintptr_t p = 0; took && p < np; p++) {
        if (p >= skip_lo && p < skip_hi) continue;
        for (;;) { uint8_t st = atomic_load(&r->state[p]);
            if (st != PG_BUSY && atomic_compare_exchange_strong(&r->state[p], &st, PG_BUSY)) { took[p] = 1; break; }
            if (st == PG_BUSY && !busy_wait(r, p)) break; }
    }
    int ok = compat_guest_remap && !compat_guest_remap((void *)r->base, r->end - r->base);
    uint64_t g = atomic_fetch_add(&g_now, 1) + 1;
    for (uintptr_t p = 0; p < np; p++) atomic_store(&r->gen[p], g);
    uint64_t l = atomic_load(&r->latest); while (l < g && !atomic_compare_exchange_weak(&r->latest, &l, g)) {}
    atomic_fetch_add(&g_now, 1);
    for (uintptr_t p = 0; took && p < np; p++) if (took[p]) atomic_store(&r->state[p], PG_NONE);
    free(took);
    LOG("[dirty] mprotect failed in pool %p..%p (%zu MiB): remapped as one mapping (%s), every page marked written; %u time(s) so far",
        (void *)r->base, (void *)r->end, (size_t)((r->end - r->base) >> 20), ok ? "done" : "FAILED", k);
    pthread_mutex_unlock(&g_defrag_mu);
}
static void unprotect_page(range_t *r, uintptr_t p) {   // 调用方已把状态从 PROT 改成 BUSY | The caller has already changed the state from PROT to BUSY
    if (mp(blk_addr(r, p), blk_len(r, p, 1), PROT_READ | PROT_WRITE)) defrag(r, p, p + 1);   /* this page is ours (busy): the remap makes it writable too */
    uint64_t g = atomic_fetch_add(&g_now, 1) + 1, l = atomic_load(&r->latest);
    atomic_store(&r->gen[p], g);
    while (l < g && !atomic_compare_exchange_weak(&r->latest, &l, g)) {}
    /* TOTK (#90): "now" moves on once more after the page's generation is stored. A checker that read "now" between the
       first step and the stores found the page clean and kept that "now" as its last look (tex_mtl's now == seen); the page
       is writable from here on, so without a newer "now" every later write went unseen until something else faulted. */
    atomic_fetch_add(&g_now, 1);
    atomic_store(&r->state[p], PG_NONE);
}
/* TOTK: a page our records call writable that keeps faulting (seen as a hang in a cutscene: one thread at 100 % in the
   signal handler). Something outside the tracker changed its mapping. After 256 faults in a row on the page: log what
   the mapping is, make it writable again; if it still faults, give up (the fault becomes a crash report, not a hang). */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void maps_line(uintptr_t a, char *out, size_t cap) {   /* the /proc/self/maps line holding a (signal-safe calls only) */
    out[0] = 0; int fd = open("/proc/self/maps", O_RDONLY | O_CLOEXEC); if (fd < 0) return;
    char buf[4096], line[256]; size_t n = 0; ssize_t got;
    while ((got = read(fd, buf, sizeof buf)) > 0)
        for (ssize_t i = 0; i < got; i++) {
            if (buf[i] != '\n') { if (n < sizeof line - 1) line[n++] = buf[i]; continue; }
            line[n] = 0; n = 0;
            uintptr_t lo = strtoull(line, NULL, 16), hi = strtoull(strchr(line, '-') ? strchr(line, '-') + 1 : line, NULL, 16);
            if (a >= lo && a < hi) { snprintf(out, cap, "%s", line); close(fd); return; }
        }
    close(fd);
}
static int stuck_check(range_t *r, uintptr_t p, void *uc) {
    static __thread uintptr_t last; static __thread unsigned repeats, healed;
    uintptr_t page = blk_addr(r, p);
    if (page != last) { last = page; repeats = healed = 0; return 1; }
    if (++repeats < 256) return 1;
    repeats = 0;
    char m[256]; maps_line(page, m, sizeof m);
    unsigned long esr = uc ? (unsigned long)UC_ESR((ucontext_t *)uc) : 0, pc = uc ? (unsigned long)UC_PC((ucontext_t *)uc) : 0;
    LOG("[dirty] page %p (pool %p..%p) keeps faulting on writes while tracked as writable: esr=%#lx (fault status %#lx) pc=%#lx state=%d; mapping: %s",
        (void *)page, (void *)r->base, (void *)r->end, esr, esr & 0x3f, pc, (int)atomic_load(&r->state[p]), m[0] ? m : "(none)");
    if (healed++) return 0;
    return mprotect((void *)page, blk_len(r, p, 1), PROT_READ | PROT_WRITE) == 0;
}
static int on_fault(void *addr, int is_write, void *uc) {
    (void)uc;
    range_t *r = is_write ? find((uintptr_t)addr) : NULL;
    if (!r) return 0;
    uintptr_t p = ((uintptr_t)addr - r->base) >> r->shift;
    uint8_t st = PG_PROT;
    static int test_every = -1; if (test_every < 0) { const char *e = getenv("TOTK_DIRTY_DEFRAG_TEST"); test_every = e ? atoi(e) : 0; }   /* debug: force the remap path */
    if (test_every && atomic_load(&g_faults) % test_every == test_every - 1 && atomic_compare_exchange_strong(&r->state[p], &st, PG_BUSY)) {
        defrag(r, p, p + 1); atomic_store(&r->state[p], PG_NONE); atomic_fetch_add(&g_faults, 1); return 1; }
    st = PG_PROT;
    if (atomic_compare_exchange_strong(&r->state[p], &st, PG_BUSY)) { unprotect_page(r, p); atomic_fetch_add(&g_faults, 1);
        if (g_drec_on && uc) { ucontext_t *u = uc; drec_add(blk_addr(r, p), UC_PC(u), UC_LR(u), 0); }
        return 1; }
    if (st == PG_BUSY) return busy_wait(r, p);   // 别的线程正在改这页的保护：等它改完重执行（改成了只读就再缺页一次、走上面那一支） | Another thread is changing this page's protection: wait for it to finish and re-execute (if it became read-only, it faults once more and takes the branch above)
    if (!atomic_load(&r->gen[p])) return 0;
    return stuck_check(r, p, uc);   // 刚被别的线程解掉（本次缺页发生在解之前）：重执行；从未保护过的页不是我们的 | Just unprotected by another thread (this fault happened before the unprotect): re-execute; pages that were never protected are not ours
}
static void prep_write(void *addr, size_t len) {
    uintptr_t a = (uintptr_t)addr, e = a + len;
    while (len && a < e) {
        range_t *r = find(a);
        if (!r) { a = (a + g_pg) & ~(g_pg - 1); continue; }
        uintptr_t stop = e < r->end ? e : r->end;
        for (uintptr_t p = (a - r->base) >> r->shift, q = (stop - 1 - r->base) >> r->shift; p <= q; p++) {
            for (;;) { uint8_t st = PG_PROT;   // 见 BUSY 等完要重看：对方可能是在设只读，等完这页是 PROT，照样得解 | After waiting out BUSY, look again: the other side may have been setting read-only, in which case the page is PROT after the wait and still has to be unprotected
                if (atomic_compare_exchange_strong(&r->state[p], &st, PG_BUSY)) { unprotect_page(r, p); atomic_fetch_add(&g_preps, 1); drec_add(blk_addr(r, p), dm_host_write_pc, 0, 1); break; }
                if (st != PG_BUSY || !busy_wait(r, p)) break; }
        }
        a = stop;
    }
}

/* Writes on behalf of the GPU (a rendering written back into CPU-visible memory): the bytes change without counting as
   a CPU write. Protected pages are taken to BUSY (a guest write meanwhile waits in the fault handler, then faults again
   and is recorded as usual), made writable, and protected again by nvn_dirty_gpu_write_end with their generation kept.
   Pages already unprotected are left as they are. Returns a token for nvn_dirty_gpu_write_end (NULL: nothing tracked). */
typedef struct { range_t *r; uintptr_t p0, p1; uint8_t *was; } gpu_write_tok;
void *nvn_dirty_gpu_write_begin(void *addr, size_t len) {
    if (!len) return NULL;
    range_t *r = find((uintptr_t)addr); if (!r || (uintptr_t)addr + len > r->end) return NULL;
    uintptr_t a = (uintptr_t)addr, p0 = (a - r->base) >> r->shift, p1 = (a + len - 1 - r->base) >> r->shift;
    gpu_write_tok *t = malloc(sizeof *t); uint8_t *was = calloc(p1 - p0 + 1, 1); if (!t || !was) { free(t); free(was); return NULL; }
    *t = (gpu_write_tok){ r, p0, p1, was };
    for (uintptr_t p = p0; p <= p1; p++) {
        for (;;) { uint8_t st = PG_PROT;
            if (atomic_compare_exchange_strong(&r->state[p], &st, PG_BUSY)) {
                if (mp(blk_addr(r, p), blk_len(r, p, 1), PROT_READ | PROT_WRITE)) atomic_store(&r->state[p], PG_PROT);   /* left protected: the write faults and the fault path remaps the pool */
                else was[p - p0] = 1;
                break; }
            if (st != PG_BUSY || !busy_wait(r, p)) break; }
    }
    return t;
}
void nvn_dirty_gpu_write_end(void *tok) {
    gpu_write_tok *t = tok; if (!t) return;
    int failed = 0;
    for (uintptr_t p = t->p0; p <= t->p1; p++) if (t->was[p - t->p0]) {
        if (mp(blk_addr(t->r, p), blk_len(t->r, p, 1), PROT_READ)) { atomic_store(&t->r->state[p], PG_NONE); failed = 1; }
        else atomic_store(&t->r->state[p], PG_PROT); }
    if (failed) defrag(t->r, 0, 0);
    free(t->was); free(t);
}

void nvn_dirty_add_range(const void *mem, size_t size) {
    static int inited;
    if (!inited) { inited = 1; g_pg = (uintptr_t)getpagesize(); g_shift = __builtin_ctzl(g_pg); mach_timebase_info(&g_tb); dm_fault_hook_register(on_fault); dm_guest_write_prep_register(prep_write); g_drec_on = getenv("DM_NVN_TILELOG") != NULL; }
    atomic_fetch_add(&g_adds_total, 1);
    pthread_mutex_lock(&g_slot_mu);
    int n = atomic_load(&g_nr);
    int slot = -1;
    {   /* TOTK: a pool created over memory that another live pool already covers must not get a second tracking range: find() returns
           the first live range for an address, so a fault on a page protected under the older range would be looked up in the newer one
           (state NONE, generation 0) and reported as a crash. Inside an existing range the pages are tracked already; any other overlap
           leaves the new pool untracked (its textures are treated as always dirty, which is safe). */
        uintptr_t nb = (uintptr_t)mem & ~(g_pg - 1), ne = ((uintptr_t)mem + size + g_pg - 1) & ~(g_pg - 1);
        for (int i = 0; i < n; i++) {
            if (!atomic_load_explicit(&g_r[i].live, memory_order_acquire) || ne <= g_r[i].base || nb >= g_r[i].end) continue;
            static _Atomic int said; int expected = 0;
            if (atomic_compare_exchange_strong(&said, &expected, 1))
                LOG("脏页跟踪：池 %p+%#zx 与已跟踪区间 %#lx-%#lx 重叠，%s", mem, size, (unsigned long)g_r[i].base, (unsigned long)g_r[i].end,
                    nb >= g_r[i].base && ne <= g_r[i].end ? "已由该区间跟踪" : "不另行跟踪");
            pthread_mutex_unlock(&g_slot_mu); return;
        }
    }
    for (int i = 0; i < n; i++) if (!atomic_load_explicit(&g_r[i].live, memory_order_acquire)) { slot = i; break; }
    if (slot < 0) {
        if (n >= MAX_RANGE) {
            int expected = 0;
            if (atomic_compare_exchange_strong(&g_overflow_logged, &expected, 1)) LOG("【红】脏页跟踪区间已满（%d），池 %p 不跟踪", MAX_RANGE, mem);
            pthread_mutex_unlock(&g_slot_mu); return;
        }
        slot = n;
    }
    { int lv = atomic_fetch_add(&g_live, 1) + 1, pk = atomic_load(&g_live_peak); while (lv > pk && !atomic_compare_exchange_weak(&g_live_peak, &pk, lv)) {} }
    range_t *r = &g_r[slot];
    uintptr_t base = (uintptr_t)mem & ~(g_pg - 1), end = ((uintptr_t)mem + size + g_pg - 1) & ~(g_pg - 1);
    unsigned shift = (unsigned)g_shift;
    if (mtl_pool_zero_copy && mtl_pool_zero_copy(mem)) {
        shift = 16 > g_shift ? 16 : (unsigned)g_shift; }   /* zero-copy pools: 64 KiB blocks */
    size_t np = (end - base + ((uintptr_t)1 << shift) - 1) >> shift;
    _Atomic uint8_t *state = r->state;
    _Atomic uint64_t *gen = r->gen;
    if (r->cap < np) {
        state = calloc(np, 1); gen = calloc(np, sizeof *gen);
        if (!state || !gen) die("脏页表分配失败（%zu 页）", np);
        // 旧数组不 free：信号处理里可能仍有极短暂的陈旧读者，泄漏量 = 每页 9 字节。
        // The old array is not freed: the signal handler may still have a very brief stale reader; leak = 9 bytes per page.
        r->cap = np;
    } else {
        memset((void *)state, 0, np); memset((void *)gen, 0, np * sizeof *gen);
    }
    r->base = base; r->end = end; r->shift = shift; r->state = state; r->gen = gen; atomic_store(&r->latest, 0);
    if (slot == n) atomic_store(&g_nr, n + 1);
    atomic_fetch_add(&g_index_gen, 1);   /* the published index is stale before the range goes live: find() scans until the rebuild */
    atomic_store_explicit(&r->live, 1, memory_order_release);
    range_index_rebuild();
    pthread_mutex_unlock(&g_slot_mu);
}
// 池销毁：先把仍受保护的页还原成可写（池内存归还游戏后可能另作他用），再标失效
// Pool destruction: first restore still-protected pages to writable (the pool memory may be reused for something else once returned to the game), then mark invalid
void nvn_dirty_remove_range(const void *mem) {
    pthread_mutex_lock(&g_slot_mu);
    range_t *r = find((uintptr_t)mem);
    if (r) { prep_write((void *)r->base, r->end - r->base); atomic_store_explicit(&r->live, 0, memory_order_release); atomic_fetch_sub(&g_live, 1); range_index_rebuild(); }
    pthread_mutex_unlock(&g_slot_mu);
}
int nvn_dirty_tracked(const void *addr, size_t len) {
    range_t *r = find((uintptr_t)addr); return r && (uintptr_t)addr + len <= r->end;
}
// 设只读：先把页标成 BUSY，mprotect 落地后才标 PROT（次序与不变式见文件头的状态说明）
// Set read-only: mark the page BUSY first, and mark it PROT only after mprotect has landed (for the order and the invariant see the state notes at the top of the file)
void nvn_dirty_protect(const void *addr, size_t len) {
    /* g_slot_mu from the lookup to the last mprotect (nxnative af616f54, PR #686 there): pool retirement and creation
       hold it too, so a range cannot be retired (its protected pages made writable, the range marked dead) or its slot
       reused between the lookup and the page states below. Without it a retirement finishing in that window left the
       retired storage read-only with no live range to release it (a host read() into it failed with EFAULT; a game
       write faulted with no owner). The page-fault and host pre-write paths still take no lock, so a BUSY page they
       hold finishes while this waits (busy_wait). */
    pthread_mutex_lock(&g_slot_mu);
    if (!len || !nvn_dirty_tracked(addr, len)) { pthread_mutex_unlock(&g_slot_mu); return; }
    range_t *r = find((uintptr_t)addr);
    uintptr_t a = (uintptr_t)addr, p = (a - r->base) >> r->shift, q = (a + len - 1 - r->base) >> r->shift;
    // 连续置成 PROT 的页合成一次 mprotect（逐页调用在首传大贴图时一帧上千次；选角页启动帧 5：4888 次）。
    // 已是 PROT / BUSY 的页断开连续段：不重复保护，也不碰别的线程正在改的页
    // Consecutive pages set to PROT are merged into one mprotect (per-page calls ran to over a thousand per frame when large textures were first uploaded; character select, startup frame 5: 4888 calls).
    // Pages already PROT / BUSY break a run: they are not protected again, and pages another thread is changing are left alone
    // TOTK (#90): a BUSY page is waited out (the run so far is protected first, so nothing of ours is BUSY meanwhile) and
    // looked at again. An unprotect in progress leaves the page writable with a generation the caller's stamp may already
    // cover: skipped, later writes to it never faulted and the texture stayed stale (letters missing in the glyph cache).
    uintptr_t run = 0; int open = 0;
    for (uintptr_t i = p; i <= q + 1; i++) {
        uint8_t st = PG_NONE;
        int took = i <= q && atomic_compare_exchange_strong(&r->state[i], &st, PG_BUSY);
        if (took && !open) { run = i; open = 1; }
        if (!took && open) {
            if (mp(blk_addr(r, run), blk_len(r, run, i - run), PROT_READ)) {   /* not protected: leave writable, and remap the pool (it marks every page written, so the caller sees its data as changed and protects again later) */
                for (uintptr_t k = run; k < i; k++) atomic_store(&r->state[k], PG_NONE);
                defrag(r, 0, 0);
            } else for (uintptr_t k = run; k < i; k++) atomic_store(&r->state[k], PG_PROT);
            open = 0; }
        if (!took && st == PG_BUSY && busy_wait(r, i)) i--;   // this page again
    }
    pthread_mutex_unlock(&g_slot_mu);
}
uint64_t nvn_dirty_now(void) { return atomic_load(&g_now); }
int nvn_dirty_since(const void *addr, size_t len, uint64_t stamp) {
    if (!len) return 0;
    if (!nvn_dirty_tracked(addr, len)) return 1;   // 未跟踪时保守视为脏，宁可多传也不显示旧内容 | When untracked, conservatively treat as dirty: better to upload more than to show stale content
    range_t *r = find((uintptr_t)addr);
    if (atomic_load(&r->latest) <= stamp) return 0;   // TOTK: nothing in the whole pool was written since the stamp (no per-page scan)
    uintptr_t a = (uintptr_t)addr, p = (a - r->base) >> r->shift, q = (a + len - 1 - r->base) >> r->shift;
    for (uintptr_t i = p; i <= q; i++) if (atomic_load(&r->gen[i]) > stamp) return 1;   // 上传前最后一次写的代号 ≤ 戳；之后的写必 > 戳 | The generation of the last write before upload is <= the stamp; any later write must be > the stamp
    return 0;
}
void nvn_dirty_slot_stats(int *live, int *used_slots, uint64_t *adds_total) {
    int n = atomic_load(&g_nr), active = 0;
    for (int i = 0; i < n; i++) active += atomic_load_explicit(&g_r[i].live, memory_order_acquire) != 0;
    *live = active; *used_slots = n; *adds_total = atomic_load(&g_adds_total);
}
void nvn_dirty_stats(uint64_t *faults, uint64_t *preps) { *faults = atomic_load(&g_faults); *preps = atomic_load(&g_preps); }
void nvn_dirty_mp_stats(uint64_t *calls, uint64_t *pages, uint64_t *ns) {
    static mach_timebase_info_data_t tb; if (!tb.denom) mach_timebase_info(&tb);
    *calls = atomic_load(&g_mp_calls); *pages = atomic_load(&g_mp_pages); *ns = atomic_load(&g_mp_ticks) * tb.numer / tb.denom;
}
