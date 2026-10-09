# Kernel waits: cheaper waits and releases, render-thread priority by identity, timer-spin probe

Status (2026-10-09): code and host tests only. Not built into an NRO and not measured on the console yet.

## 1. What the profile showed

A night tour (Russian edition, `rex_profile.log` of `tour_20261009_055101`, RU ELF `run/me1/ru_integ4.elf`) had one
guest thread busy at 92 % of a core in one boot. It is a guest timer thread, probably Bink's background IO:

| Role | RU address | EN address |
|---|---|---|
| CRT thread start (calls the start routine with its argument; shared by all guest threads) | `sub_82816B78` | `sub_82816348` |
| Timer loop | `sub_82D82530` | `sub_82D82730` |
| Inner loop (one call per tick) | `sub_82D823A8` | `sub_82D825A8` |
| Timer creation `(hz)` | `sub_82D825D8` | `sub_82D827D8` |
| Creates the 60 Hz and 13 Hz timers | `sub_82D7EA08` | `sub_82D7EC08` |
| Item ready test `(item, tick)` | `sub_82D7E790` | `sub_82D7E990` |
| Item fill callback `(item, tick)` | `sub_82D7E7E8` | `sub_82D7E9E8` |

(EN addresses from `editions/ru/address_map.json`; the EN code below was read from the generated code.)

The inner loop, `inner(timer)`: increments the tick (`timer+28`), then repeatedly: calls every item's ready test
(`item+16`), keeps the ready ones sorted by the test's result, collects up to 16 of them, calls
`WaitForMultipleObjects(n, item mutexes, any, 1 ms)`, runs the fill callback (`item+20`) of the item it got and
releases its mutex, and scans again. It leaves only when no item is ready (or the timer stops). The ready test:

    if ([item-228] != 0) return 0;                       // not ready
    pct = ([item-184] + 1) * 100 / ([item-188] + 1);    // 32-bit, as the guest
    if (pct < 50) return -1 - pct;                       // ready on EVERY scan, whatever the tick
    if ([item+328] != tick) return 0x80000000 - [item+328];  // ready once per tick
    return 0;

The fill callback (`owner = item - 500`) calls the function at `[owner+256]` with `owner+240` and stores the tick in
`[owner+828]` (= `[item+328]`). So while an item's fill stays under 50 % the loop never waits for the next tick; each
pass is a ready test, a wait that succeeds at once (the mutex is free) and a release. The guest does this on the
Xbox 360 too; what made it expensive here was the cost of one pass inside the SDK:

| Where | Share of the spinner's samples |
|---|---|
| `cond_.notify_all()` in `PosixCondition<Mutant>::Release` (on Horizon `svcSignalProcessWideKey`, a system call, even with nobody waiting) | 37 % |
| six malloc/free per pass: vectors in `xeNtWaitForMultipleObjectsEx`, `XObject::WaitMultiple`, `rex::thread::WaitMultiple` | 22.5 % |
| `dynamic_cast<PosixWaitHandle*>` per handle (a cross-cast, type-name `strcmp`s) | ~12 % |
| `ObjectTable::LookupObject` (global lock) | ~5 % |

And the profiler's priority rule (`masseffect_exclusive_core` 20 + K) raised "the second-hottest guest thread" to
0x2B: in that boot it was this spinner, which then ran above the ring thread instead of UE3's render thread.

## 2. What changed (all guest-visible behaviour unchanged)

### 2.1 Notify only when someone may wait (`sdk/src/core/threading_posix.cpp`)

`PosixConditionBase` counts the threads inside `Wait()` (`cond_waiters_`, guarded by the object's `mutex_`):
incremented under `mutex_` after the predicate failed and before `cond_.wait*` releases the mutex, decremented under
`mutex_` after the wait returns. Every state change that used to notify unconditionally (Event `Signal`, Semaphore
`Release` and `post_execution`, Mutant `Release`, Timer `Signal`) now reads the count in the same critical section
that changes the state and calls `cond_.notify_all()` only when it is not zero (still after unlocking, as before).

Why no wake-up can be lost: the waiter checks the predicate and registers under `mutex_`, and `cond_.wait*` releases
`mutex_` atomically with parking. The signaller changes the state and reads the count under the same `mutex_`. Either
the signaller's critical section comes first (the waiter's predicate sees the new state and does not park) or the
waiter's comes first (the signaller sees the count and notifies a thread that is already on the condition variable).
`NotifyMulti` (wake points of parked `WaitMultiple` calls) was already gated by its own registration count and is
unchanged; so are `InterruptWaiters` and the thread-exit notify (rare).

### 2.2 No allocation, no dynamic_cast per wait

- `WaitHandle` (`sdk/include/rex/thread.h`) has a `platform_wait_object()` pointer that every
  `PosixConditionHandle` sets in its constructor to its `PosixConditionBase`. `ConditionOf()` uses it with a static
  cast; `dynamic_cast` stays only as a fallback for a handle that did not set it (none today). `Wait`,
  `SignalAndWait` and `WaitMultiple` use it.
- `rex::thread::WaitMultiple`: the conditions array is on the stack up to 64 handles (the kernel's
  `MAXIMUM_WAIT_OBJECTS`), heap above; the alertable loop no longer copies it per slice. `PosixConditionBase::
  WaitMultiple` takes `(pointer, count)` and tracks the locks it holds with a counter instead of a
  `std::vector<std::unique_lock>`.
- One handle: `rex::thread::WaitMultiple` calls the same code as `Wait()` (`WaitOne`), which is exactly what the
  general path did for one handle (no wake point, `Wait()` on the handle, same alertable loop and counters), minus
  the vector and the unused `WakePoint`.
- `XObject::WaitMultiple` (`sdk/src/system/xobject.cpp`): the `WaitHandle*` array is on the stack (heap above 64).
- `xeNtWaitForMultipleObjectsEx` and `KeWaitForMultipleObjects` (`sdk/src/kernel/xboxkrnl/xboxkrnl_threading.cpp`):
  `WaitObjectList` holds the retained objects on the stack (heap above 64) and releases them on every return path,
  like the `std::vector<object_ref<XObject>>` it replaces. Same lookup order, same early `X_STATUS_INVALID_PARAMETER`.

`ObjectTable::LookupObject` is unchanged: it takes the global critical region, which the handle table needs.

### 2.3 Render thread chosen by what it runs (`sdk/src/ui/switch_perf.cpp`)

The game hooks now tell the profiler which thread is which (`RexSwitchPerfNoteThreadRole`):

- role 0 from the `UGameEngine::Tick` hook (`me_console_exec.cpp`, EN and RU overlay): the game's main thread;
- role 1 from the D3D Swap throttle hook (`sub_8222C768` with reason 3, `me_ring_wait.cpp`, EN and RU overlay), which
  only UE3's render thread calls: one count per Swap in that thread's profiler slot.

`ApplyExclusiveCore`:

- pins the game thread (role 0) once it uses 60 % of a core; without role marks (a build without those hooks) it
  falls back to the old rule (hottest "XThread...");
- with 20 + K, raises to 0x2B the thread with the most Swaps in the last report interval (at least 10), never the
  pinned thread, regardless of its CPU share. If the role moves to another thread later, the old one gets its saved
  priority back. Without Swap marks nothing is raised (logged once: `-- render thread not identified yet`).
- Both choices are logged: `-- exclusive core K for "..." (game thread: runs UGameEngine::Tick; CPU ..., guest entry
  0x...)` and `-- priority 0x2B for "..." (render thread: N guest D3D Swaps in the interval; CPU ..., guest entry
  0x..., was priority 0x...)`.

Every guest thread also reports its guest start routine and argument (`RexSwitchPerfSetCurrentGuestEntry` from
`XThread::Create`'s thread body); the thread header lines of `rex_profile.log` end with `| guest entry 0x........
ctx 0x........` (and `| N guest Swaps` for the render thread), so a thread can be recognised across boots. The parsers
(`me1_tour_summary.py`, `symbolize_profile.py`) match only the start of that line and are unaffected.

### 2.4 Probe of the spinner (`app/src/native/me_timer_spin_diag.cpp`, RU overlay copy)

`masseffect_diag_timer_spin = N` (int, default 0 = off): hooks the ready test (EN `sub_82D7E990`, RU `sub_82D7E790`);
per thread and item it counts the ready tests with the same tick, and past N logs once per second per item:

    [timer_spin] item XXXXXXXX tick T | C ready tests with this tick, R/s | [item-228] ........ [item-188] A
    [item-184] B -> pct P | [item+328] L -> ready ........ | fill fn FFFFFFFF(owner+240)

It reads guest memory and returns the original's result untouched. With the cvar at 0 the hook costs one load and a
branch per ready test. **This hook changes the hooked set**: the next build regenerates the code (`tools/edition.sh`
does it for every edition; for a plain EN build run `tools/codegen.sh`). It is the only new guest address in the
sources.

No fix of the game's loop is included: whether "under 50 % full, keep polling" ends depends on the fill function at
`[owner+256]` and on the IO it waits for, which the probe is there to show. A change of the loop would need that proof
first; it would go behind its own cvar, default off.

## 3. Expected effect

- Per pass of the spinner: the notify (37 %), the six allocations (22.5 %) and the dynamic_cast (~12 %) are gone,
  about 70 % of the pass. The loop itself stays a busy loop as long as the game's condition holds (it will run more
  passes per second, not fewer cycles), so its own core time does not drop by itself. What drops: system calls into
  the Horizon scheduler (one `svcSignalProcessWideKey` per pass) and traffic on newlib's global malloc lock, both
  shared with every other thread, and the time the spinner holds the CPU per useful pass.
- Every other guest wait and release in the game gets the same savings (every `KeSetEvent`/`ReleaseMutex`/
  `ReleaseSemaphore` with nobody waiting no longer makes a system call; no allocation per multi-object wait).
- The priority fix removes the case where the spinner ran at 0x2B above the ring thread; with
  `masseffect_exclusive_core` 20 + K the raised thread is now always the render thread.

To measure on the console (same config, cold, `tools/switch_cycle.sh`): fps per location, the spinner's CPU and
samples, the `-- priority 0x2B` line names the render thread, the ring thread's CPU; then a run with
`masseffect_diag_timer_spin = 1000` to see the item state while it spins.

## 4. Tests

`tests/cpu/test_kernel_waits.cpp` (run by `tests/run_all.sh kernel_waits`) compiles `threading_posix.cpp` on the
host (macOS or Linux) with stubs for the logger, the cvar registry and the timer queue, and checks, with
`thread_wait_blocking` on and off: no lost wake-up when Set/Release races the start of a blocking or timed wait
(Event, Semaphore with several waiters, Mutant hand-off), mutual exclusion and recursion of a mutant acquired through
one-handle `WaitAny(1 ms)` from four threads (the spinner's pattern), WaitAny indexes, WaitAll, 70 handles (heap
fallback), one-handle timeouts, alertable waits, SignalAndWait, and that every handle type records its condition.
Negative control: with the Event notify forced off the test hangs in the first race and its 120 s watchdog fails it.
Also clean under ThreadSanitizer (`-fsanitize=thread`, macOS arm64).
