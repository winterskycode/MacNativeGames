/* One-frame-chain timeline (see totk_timeline.h). */
#include "totk_timeline.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct { uint64_t t, a, b; uint32_t kind, tid; uint64_t frame; } tl_ev;
enum { TL_CAP = 1 << 20 };
static tl_ev *g_ev;
static _Atomic uint64_t g_n, g_commit, g_frame;
static _Atomic int g_state = -1;   /* -1 unread, 0 off, 1 waiting for the window, 2 recording, 3 written */
static uint64_t g_first, g_count;
static char g_path[1024];
/* TOTK Android: <first frame> "go" (TOTK_TIMELINE=<file>:go:<frames>) waits for <file>.go instead; each time that file
   appears (holding a tag), a window of <frames> starts there and is written to <file>-<tag> (bench.py --timeline: one
   window in each scene's flight). */
static int g_trigger;
static char g_out[1100];

static void tl_init(void) {
    const char *e = getenv("TOTK_TIMELINE");
    int st = 0;
    if (e && *e) {
        const char *c1 = strrchr(e, ':');
        const char *c0 = c1 ? c1 - 1 : NULL;
        while (c0 && c0 > e && *c0 != ':') c0--;
        if (c1 && c0 && c0 > e && *c0 == ':' && (size_t)(c0 - e) < sizeof g_path) {
            memcpy(g_path, e, (size_t)(c0 - e)); g_path[c0 - e] = 0;
            g_first = strtoull(c0 + 1, NULL, 10); g_count = strtoull(c1 + 1, NULL, 10);
            g_trigger = !strncmp(c0 + 1, "go:", 3);
            snprintf(g_out, sizeof g_out, "%s", g_path);
            g_ev = calloc(TL_CAP, sizeof *g_ev);
            if (g_ev && g_count) st = 1;
        }
        if (!st) fprintf(stderr, "[timeline] TOTK_TIMELINE=%s 读不懂（要 <文件>:<首帧>:<帧数>），不记录\n", e);
        else fprintf(stderr, "[timeline] 记录第 %llu 帧起 %llu 帧的时间线到 %s\n", (unsigned long long)g_first, (unsigned long long)g_count, g_path);
    }
    int want = -1; atomic_compare_exchange_strong(&g_state, &want, st);
}
int totk_tl_on(void) {
    int s = atomic_load_explicit(&g_state, memory_order_relaxed);
    if (s < 0) { tl_init(); s = atomic_load(&g_state); }
    return s == 2;
}
void totk_tl_at(int kind, uint64_t t_ns, uint64_t a, uint64_t b) {
    if (!totk_tl_on()) return;
    /* TOTK Android: TOTK_TIMELINE_LEAN=1 keeps only what latency_report.py reads: the buffer otherwise fills in ~250 game frames
       (TL_REC_COPY alone is ~3,500 events a frame), far short of a latency window */
    { static int lean = -1; if (lean < 0) { const char *e = getenv("TOTK_TIMELINE_LEAN"); lean = e && atoi(e) > 0; }
      if (lean && kind != TL_INPUT && kind != TL_SHOWN && kind != TL_PIXELS && kind != TL_PRES_IN && kind != TL_PRES_OUT) return; }
    uint64_t i = atomic_fetch_add_explicit(&g_n, 1, memory_order_relaxed);
    if (i >= TL_CAP) return;
    uint64_t tid; pthread_threadid_np(NULL, &tid);
    g_ev[i] = (tl_ev){t_ns, a, b, (uint32_t)kind, (uint32_t)tid, atomic_load_explicit(&g_frame, memory_order_relaxed)};
}
void totk_tl(int kind, uint64_t a, uint64_t b) {
    if (!totk_tl_on()) return;
    totk_tl_at(kind, clock_gettime_nsec_np(CLOCK_UPTIME_RAW), a, b);
}
uint64_t totk_tl_commit_id(void) { return atomic_fetch_add(&g_commit, 1) + 1; }

static void tl_write(void) {
    FILE *f = fopen(g_out, "w");
    if (!f) { fprintf(stderr, "[timeline] cannot write %s\n", g_out); return; }
    uint64_t n = atomic_load(&g_n); if (n > TL_CAP) n = TL_CAP;
    fprintf(f, "# t_ns\tkind\ttid\tframe\ta\tb\n");
    static const char *names[] = {"?", "sub_in", "sub_out", "rep_begin", "rep_end", "commit", "gpu", "wait_in", "wait_out",
                                  "acq_in", "acq_out", "pres_in", "pres_out", "finish_in", "finish_out", "fence", "rs_join",
                                  "pass_begin", "pass_end", "gw_in", "gw_out", "gwake", "cw_in", "cw_out", "wb_in", "wb_out", "wb_step",
                                  "input", "shown", "pixels", "rec_end", "rec_begin", "rec_copy"};
    for (uint64_t i = 0; i < n; i++) {
        const tl_ev *e = &g_ev[i];
        fprintf(f, "%llu\t%s\t%u\t%llu\t%llu\t%llu\n", (unsigned long long)e->t, e->kind < sizeof names / sizeof *names ? names[e->kind] : "?",
                e->tid, (unsigned long long)e->frame, (unsigned long long)e->a, (unsigned long long)e->b);
    }
    fclose(f);
    fprintf(stderr, "[timeline] wrote %llu events to %s\n", (unsigned long long)n, g_out);
}
void totk_tl_frame(uint64_t presents) {
    totk_tl_on();
    atomic_store(&g_frame, presents);
    int s = atomic_load(&g_state);
    if (s == 1 && g_trigger) {   /* look for <file>.go every 15 frames */
        if (presents % 15) return;
        char go[1100], tag[64] = "";
        snprintf(go, sizeof go, "%s.go", g_path);
        FILE *t = fopen(go, "r");
        if (!t) return;
        if (!fgets(tag, sizeof tag, t)) tag[0] = 0;
        fclose(t); remove(go);
        tag[strcspn(tag, " \r\n/")] = 0;
        snprintf(g_out, sizeof g_out, "%s-%s", g_path, tag[0] ? tag : "window");
        g_first = presents; atomic_store(&g_n, 0); atomic_store(&g_state, 2);
        fprintf(stderr, "[timeline] window %s from frame %llu\n", g_out, (unsigned long long)presents);
        return;
    }
    if (s == 1 && presents >= g_first) atomic_store(&g_state, 2);
    /* GPU feedback of the last frames arrives after their presents: keep recording 30 more frames, then write */
    else if (s == 2 && presents >= g_first + g_count + 30) { atomic_store(&g_state, 3); tl_write(); if (g_trigger) atomic_store(&g_state, 1); }
}
