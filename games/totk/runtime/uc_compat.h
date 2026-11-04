/* Signal-context registers on Darwin and Linux (Android). The macros are lvalues except UC_ESR. */
#pragma once
#include <signal.h>
#include <stdint.h>
#include <ucontext.h>
#ifdef __ANDROID__
#include <asm/sigcontext.h>
#define UC_PC(u) ((u)->uc_mcontext.pc)
#define UC_SP(u) ((u)->uc_mcontext.sp)
#define UC_LR(u) ((u)->uc_mcontext.regs[30])
#define UC_FP(u) ((u)->uc_mcontext.regs[29])
#define UC_X(u, i) ((u)->uc_mcontext.regs[i])
#define UC_XS(u) ((u)->uc_mcontext.regs)
/* ESR of the fault: the kernel's esr_context record in the signal frame's reserved area (0 when absent). */
static inline uint64_t uc_esr(const ucontext_t *u) {
    const uint8_t *p = (const uint8_t *)u->uc_mcontext.__reserved, *end = p + sizeof u->uc_mcontext.__reserved;
    while (p + sizeof(struct _aarch64_ctx) <= end) {
        const struct _aarch64_ctx *h = (const void *)p;
        if (!h->magic || !h->size) break;
        if (h->magic == ESR_MAGIC) return ((const struct esr_context *)h)->esr;
        p += h->size;
    }
    return 0;
}
#define UC_ESR(u) uc_esr(u)
#else
#define UC_PC(u) ((u)->uc_mcontext->__ss.__pc)
#define UC_SP(u) ((u)->uc_mcontext->__ss.__sp)
#define UC_LR(u) ((u)->uc_mcontext->__ss.__lr)
#define UC_FP(u) ((u)->uc_mcontext->__ss.__fp)
#define UC_X(u, i) ((u)->uc_mcontext->__ss.__x[i])
#define UC_XS(u) ((u)->uc_mcontext->__ss.__x)
#define UC_ESR(u) ((u)->uc_mcontext->__es.__esr)
#endif
