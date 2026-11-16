/* TOTK_TIMELINE=<file>:<first frame>:<frames> (diagnostic, off by default): one timeline of a run of frames on one clock,
   to see which link of the chain "game logic -> submit -> replay -> GPU -> the game waiting for that frame" sets the frame
   time. Host events are CLOCK_UPTIME_RAW nanoseconds; GPU start / end of each committed command buffer come from Metal's
   commit feedback (seconds on the same mach clock). Frames are counted by the game's presents (nvnQueuePresentTexture).
   The events go to a preallocated buffer and are written to <file> once, a few frames after the window
   (scripts/diag/timeline.py reads it). */
#pragma once
#include <stdint.h>

enum {
    TL_SUB_IN = 1, TL_SUB_OUT,      /* nvnQueueSubmitCommands enter / return (a = draws so far is not known: 0) */
    TL_REP_BEGIN, TL_REP_END,       /* replay of a submission (submit_exec), on the game thread or the translation thread */
    TL_COMMIT,                      /* a command buffer committed: a = commit id, b = 1 last of its submission, 0 mid-submission */
    TL_GPU,                         /* GPU span of commit a: t = start, b = end (ns) */
    TL_WAIT_IN, TL_WAIT_OUT,        /* nvnSyncWait that blocks: a = fence value waited for, b = 1 fence of the newest commit */
    TL_ACQ_IN, TL_ACQ_OUT,          /* nvnWindowAcquireTexture */
    TL_PRES_IN, TL_PRES_OUT,        /* nvnQueuePresentTexture */
    TL_FINISH_IN, TL_FINISH_OUT,    /* nvnQueueFinish */
    TL_FENCE,                       /* fence value of commit a: b */
    TL_RS_JOIN,                     /* the replay thread waited a ns for the encode stage (b = join count) */
    TL_PASS_BEGIN, TL_PASS_END,     /* a render pass opened / ended on the replay thread: a = pass serial, b = draws in it (end) */
    TL_GW_IN, TL_GW_OUT,            /* a guest thread blocks in the runtime (threads.c block): in a = kind << 56 | key / address, b = guest lr; out a = kind, b = result */
    TL_GWAKE,                       /* this thread wakes a blocked guest thread: a = its host thread id, b = wait kind */
    TL_CW_IN, TL_CW_OUT,            /* a guest thread waits for a virtual core (threads.c core_take) */
    TL_WB_IN, TL_WB_OUT,            /* GPU renderings written back to memory (nvn_mtl.m wb_write_list): in a = entries; out a = entries, b = 1 a read-back blit was among them */
    TL_WB_STEP,                     /* inside it: a = 1 the renderings' fences passed (b = fence), 2 the uploads finished, 3 a texture read back, all levels and layers (b = serial) */
    TL_INPUT,                       /* TOTK Android: the game read the controller (a = buttons, b = right stick x); a = 1 << 40 | axis: input.now set that stick to b */
    TL_SHOWN,                       /* TOTK Android: a picture reached the screen (VK_GOOGLE_display_timing): t = actual
                                       present time, a = present id, b = when it was presented (same clock) */
    TL_PIXELS,                      /* TOTK Android (TOTK_LATENCY_PROBE): how much picture a changed from the one before (b) */
    TL_REC_END,                     /* TOTK Android: nvnCommandBufferEndRecording (a = bytes recorded; the thread is the recording thread) */
    TL_REC_BEGIN,                   /* TOTK Android: nvnCommandBufferBeginRecording (a = the command buffer) */
    TL_REC_COPY,                    /* TOTK Android: CopyCommands / CallCommands inlines handle a into command buffer b */
    TL_PLAN,                        /* TOTK Android: a present's requested time (VK_GOOGLE_display_timing): a = present id, b = desired time (0 none) */
    TL_READY,                       /* TOTK Android: a present's GPU work finished: t = its copy to the screen image, a = present id, b = the work before it */
    TL_MARK,                        /* TOTK Android: a step of the frame hand-off (a = TL_MARK_* below, b = a value of that step) */
};
/* TL_MARK a: the submit call (game thread) and the start of its replay (translation and recording threads) */
enum { TL_MARK_DEFERRED = 1,        /* deferred texture handles read */
       TL_MARK_CAPTURE_INDEX,       /* the uniform capture's ranges and buffer are ready */
       TL_MARK_HANDED_OFF,          /* the submission is queued for the translation thread */
       TL_MARK_CAPTURE_BYTES,       /* the captured bytes are in */
       TL_MARK_REPLAY_READY,        /* the replay installed the capture and may start on the first op */
       TL_MARK_CAPTURE_WAIT_IN,     /* the translation thread waits for the captured bytes */
       TL_MARK_CAPTURE_WAIT_OUT,
       TL_MARK_QUEUE_SUBMIT,        /* vkQueueSubmit of recorded work, on the thread that calls the driver (b = 1 with a fence) */
       TL_MARK_FIRST_DRAW,          /* the submission's first draw reaches draw_graphics */
       TL_MARK_SLOT_WAIT_IN,        /* beginning a batch waits for one of the batch slots (b = 1 for the present's batch) */
       TL_MARK_SLOT_WAIT_OUT,
       TL_MARK_DRAIN_IN,            /* the present waits for the recording thread to replay what was published */
       TL_MARK_DRAIN_OUT,
       TL_MARK_PRESENT_CALL_IN,     /* vkQueuePresentKHR */
       TL_MARK_PRESENT_CALL_OUT,
       TL_MARK_GPU_WAIT_IN,         /* a CPU wait for a fence that has not passed (b = the caller's offset in its library) */
       TL_MARK_GPU_WAIT_OUT,
       TL_MARK_JOB,                 /* the translation thread starts a job (b = 1 submit, 2 present, 3 texture release, 4 plan) */
       TL_MARK_JOB_END,
       TL_MARK_ACQUIRE_IN,          /* vkAcquireNextImageKHR */
       TL_MARK_ACQUIRE_OUT,
       TL_MARK_PRESENT_ONE_IN,      /* one picture's present, from the blit's recording to the driver's present (b = 1 a generated or repeated picture) */
       TL_MARK_PRESENT_ONE_OUT };
#define TL_INPUT_SCRIPTED (1ull << 40)   /* TL_INPUT a: input.now set stick axis (a & 0xff) to b (latency_report.py: a >> 40) */

int totk_tl_on(void);                                   /* the window is open (cheap after the first call) */
void totk_tl(int kind, uint64_t a, uint64_t b);         /* event now, on this thread */
void totk_tl_at(int kind, uint64_t t_ns, uint64_t a, uint64_t b);
/* Delayed submit/feedback events belong to their recorded submission frame;
   the presenting thread may already have advanced the global frame counter. */
void totk_tl_at_frame(int kind, uint64_t t_ns, uint64_t frame, uint64_t a, uint64_t b);
void totk_tl_frame(uint64_t presents);                  /* the game presented: counts frames, opens / closes the window, writes the file */
uint64_t totk_tl_commit_id(void);                       /* id for a command buffer about to be committed */
/* TOTK_TIMELINE=<file>:trigger waits indefinitely. The submit thread starts
   this one-shot window with the same first/count as its GPU counters. */
int totk_tl_trigger(uint64_t first, uint64_t count);
