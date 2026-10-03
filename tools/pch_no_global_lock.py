#!/usr/bin/env python3
"""Guest interrupt-disable brackets without the host global lock, switchable at run time (run
after every codegen, like tools/pch_no_volatile.py and tools/direct_calls.py).

The guest wraps its atomic read-modify-write sequences in `mfmsr` + `mtmsrd r13,1` ... `mtmsrd r8,1`. The
codegen turns that into REX_CHECK_GLOBAL_LOCK / REX_ENTER_GLOBAL_LOCK / REX_LEAVE_GLOBAL_LOCK on the SDK's
recursive global_critical_region mutex, which the kernel emulation, file system, audio and MMIO code also
take. In this game all 193 brackets (96 functions) surround one ldarx/lwarx ... stdcx./stwcx. pair, and the
codegen already emits stwcx./stdcx. as a compare-and-swap, so atomicity does not depend on the lock; the
kernel's spinlocks are compare-and-swap too (xeKeKfAcquireSpinLock). With g_me_lockfree_atomics set
(cvar masseffect_lockfree_atomics, read in MassEffectApp::OnPreSetup) the brackets skip the lock.
Idempotent.

  usage: python3 tools/pch_no_global_lock.py [--gen DIR] [--dry-run|--check] [pch path]
  (default app/generated/default/masseffect_pch.h; exit 1 if a pattern is missing; see tools/pch_common.py)
"""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pch_common as c

NAME = 'pch_no_global_lock'
args = c.Args()
path = args.file or os.path.join(args.gen, 'masseffect_pch.h')
if not os.path.isfile(path):
    c.failure('%s: %s not found' % (NAME, path))
s = c.read(path)
if 'g_me_lockfree_atomics' in s:
    c.finish(args, path, s, True, NAME)

old_check = '''#define REX_CHECK_GLOBAL_LOCK()                                        \\
  ([&]() -> u64 {                                                      \\
    auto lock_ = rex::thread::global_critical_region::AcquireDirect(); \\
    return ppc_global_lock_count_().load() ? 0 : 0x8000;               \\
  }())'''
new_check = '''// Mass Effect: tools/pch_no_global_lock.py. Set before any guest code runs; never changes after.
extern "C" bool g_me_lockfree_atomics;

#define REX_CHECK_GLOBAL_LOCK()                                          \\
  ([&]() -> u64 {                                                        \\
    if (g_me_lockfree_atomics) return 0x8000;                        \\
    auto lock_ = rex::thread::global_critical_region::AcquireDirect();   \\
    return ppc_global_lock_count_().load() ? 0 : 0x8000;                 \\
  }())'''
old_enter = '''#define REX_ENTER_GLOBAL_LOCK()                          \\
  do {                                                   \\
    rex::thread::global_critical_region::mutex().lock(); \\
    ppc_global_lock_count_().fetch_add(1);               \\
  } while (0)'''
new_enter = '''#define REX_ENTER_GLOBAL_LOCK()                          \\
  do {                                                   \\
    if (g_me_lockfree_atomics) break;                \\
    rex::thread::global_critical_region::mutex().lock(); \\
    ppc_global_lock_count_().fetch_add(1);               \\
  } while (0)'''
old_leave = '''#define REX_LEAVE_GLOBAL_LOCK()                                                           \\
  do {                                                                                    \\
    auto old_count_ = ppc_global_lock_count_().fetch_sub(1);                              \\'''
new_leave = '''#define REX_LEAVE_GLOBAL_LOCK()                                                           \\
  do {                                                                                    \\
    if (g_me_lockfree_atomics) break;                                                 \\
    auto old_count_ = ppc_global_lock_count_().fetch_sub(1);                              \\'''
for a, b in ((old_check, new_check), (old_enter, new_enter), (old_leave, new_leave)):
    if a not in s:
        c.failure('%s: pattern not found in %s: %s' % (NAME, path, a.split('\n')[0]))
    s = s.replace(a, b, 1)
c.finish(args, path, s, False, NAME)
