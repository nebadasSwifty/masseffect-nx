/**
 * @file        switch_thread_snapshot.h
 * @brief       One-shot snapshot of every host thread (Switch), for hang diagnostics
 *
 * Implemented in switch_perf.cpp, which already knows every thread handle (it wraps libnx's
 * threadCreate/threadClose). Each thread is paused in turn (svcSetThreadActivity), its context is
 * read (svcGetThreadContext3) together with the return addresses of its x29 frame chain, and it is
 * resumed. Nothing that could take a lock runs while a thread is paused. The caller's own thread is
 * listed without a context.
 *
 * Host addresses inside the executable are reported relative to the image base
 * (RexSwitchImageBase); addr2line on the ELF of the same build turns them into function names.
 * Recompiled guest functions keep their guest address in their name (__imp__sub_XXXXXXXX).
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define REX_SWITCH_SNAPSHOT_FRAMES 48

enum RexSwitchSnapshotState {
  kRexSwitchSnapshotOk = 0,
  kRexSwitchSnapshotPauseFailed = 1,    /* already suspended by the SDK, or exiting */
  kRexSwitchSnapshotContextFailed = 2,  /* paused, but svcGetThreadContext3 failed */
  kRexSwitchSnapshotSelf = 3,           /* the calling thread: no context */
};

typedef struct RexSwitchThreadSnapshot {
  uint32_t handle;
  int32_t state;       /* RexSwitchSnapshotState */
  int32_t in_kernel;   /* 1: the pc sits on/after an svc (blocked in the kernel) */
  uint32_t frame_count;
  char name[32];
  uint64_t pc;
  uint64_t lr;
  uint64_t sp;
  uint64_t fp;
  uint64_t frames[REX_SWITCH_SNAPSHOT_FRAMES]; /* return addresses along the x29 chain */
} RexSwitchThreadSnapshot;

/* Fills up to max entries, returns how many were written. Safe to call from any thread. */
size_t RexSwitchSnapshotThreads(RexSwitchThreadSnapshot* out, size_t max);

/* Base of the executable image and end of its code segment (host addresses). */
uint64_t RexSwitchImageBase(void);
uint64_t RexSwitchImageTextEnd(void);

#ifdef __cplusplus
}
#endif
