# Bug Report — Xeno.cpp & XenoUI.cpp

---

## Critical

### [C++ #1] Heap corruption — `Compilable` export (Xeno.cpp line 1061, XenoUI.cpp line 441)
`Compilable()` allocates the return string with `new char[]` (C++ heap), but the C# caller
frees it with `Marshal.FreeCoTaskMem()` (COM heap). These are different allocators.
This causes silent heap corruption or a crash on every compile check.
**Fix:** allocate with `CoTaskMemAlloc` in C++ so `FreeCoTaskMem` is the correct free.

### [C++ #2] Null pointer used before check — `playersAddr` (Xeno.cpp line 563)
`DM.FindClassAddr("Players")` can return 0 if the Players service hasn't loaded yet.
The result is immediately used in `read_mem<...>(playersAddr + off::LocalPlayer, _h)`,
reading from address `0x110` — an almost certain access violation.
**Fix:** check `playersAddr != 0` before the spin loop.

### [C++ #3] Data race on `g_initing` (Xeno.cpp lines 975–980)
`AttachClient` runs in a detached thread and calls `g_initing.insert(pid)` /
`g_initing.erase(pid)` without holding any lock. `WatchClients` reads `g_initing`
under `g_clientsMtx`. Concurrent modification with no shared lock = undefined behaviour.
**Fix:** protect all `g_initing` accesses with `g_clientsMtx`.

### [C++ #4] Dangling HANDLE in detached revert thread (Xeno.cpp line 365)
`SetBytecode` spawns a detached `std::thread` that fires 850 ms later and calls
`write_mem` using the captured HANDLE `hh`. If the owning `RBXClient` is destroyed
before 850 ms elapses, `CloseHandle(_h)` runs in the destructor and `hh` becomes
a closed handle. The thread then writes through it.
**Fix:** capture a `std::weak_ptr<RBXClient>` and lock it inside the thread before use
(requires restructuring to pass the shared_ptr; documented here as a known issue).

---

## High

### [C++ #5] Broken path-traversal guard — `InDir` (Xeno.cpp lines 753–760)
The hand-rolled `..` resolver does not handle absolute `rel` paths, symlinks, or
`//`-style paths, and its parent-path walking is logically incorrect (it modifies `r`
from `b`, not from `rel`). A crafted path can escape the workspace root.
**Fix:** use `std::filesystem::weakly_canonical` on `base / rel` and prefix-compare
against the canonical base.

### [C++ #6] `CheckCompile` — undefined behaviour on empty string (Xeno.cpp line 275)
`bc[0]` is accessed without first checking `bc.empty()`. If `Luau::compile` returns
an empty string, this is out-of-bounds access (UB).
**Fix:** guard with `!bc.empty()` before indexing.

### [C# #1] Save timer fires before WebView2 is ready (XenoUI.cpp line 67)
`_saveTimer.Start()` runs in the constructor. The `CoreWebView2` object is not yet
initialised (it completes asynchronously). The first timer tick calls `GetContent()`
which calls `ExecuteScriptAsync` on a null `CoreWebView2` → NullReferenceException.
**Fix:** start `_saveTimer` only after `EnsureCoreWebView2Async` completes.

### [C# #2] Scripts encoded as ASCII, not UTF-8 (XenoUI.cpp line 439)
`Encoding.ASCII.GetBytes(script)` silently replaces every non-ASCII byte with `?`.
Any Lua script containing a Unicode comment, string literal, or non-Latin identifier
is corrupted before it reaches the DLL.
**Fix:** use `Encoding.UTF8.GetBytes(script)` (matches the `Execute` call which
already uses UTF-8 on line 434).

---

## Medium

### [C++ #7] `GetTickCount ^ PID` — predictable port (Xeno.cpp line 1028)
Port selection uses `srand(GetTickCount()^self)` then `rand()`. `GetTickCount` has
~15 ms resolution and the PID is visible in Task Manager. Another process on the
same machine can enumerate the port in < 1 s.
**Fix:** replace with `BCryptGenRandom` or `rand_s` for the port value.

### [C++ #8] Infinite spin on `SetForegroundWindow` (Xeno.cpp line 666)
`while (GetForegroundWindow() != hwnd) { SetForegroundWindow(hwnd); Sleep(5); }`
will never terminate if Windows refuses the foreground switch (e.g., the user is
in full-screen exclusive mode or a UAC prompt is active).
**Fix:** add an iteration cap / timeout (e.g., 200 attempts ≈ 1 s).

### [C# #3] Port resolved on a background thread without memory barrier (XenoUI.cpp line 308)
`ServerUrl` (a plain `string` field) is written from `Task.Run` and read from the
UI thread with no lock or `volatile`. On x86 this is usually safe due to TSO, but
it is technically a data race under the .NET memory model.
**Fix:** write `ServerUrl` via `Dispatcher.Invoke` so it is always set on the UI thread.

### [C# #4] `GetClientsFromDll` walks pointer with no upper bound (XenoUI.cpp line 383)
The sentinel-terminated loop has no maximum iteration count. A DLL bug or memory
corruption could produce a run that never hits a null `name`, looping until a crash.
**Fix:** add a reasonable cap (e.g., 64 iterations).

### [C++ #9] `NtUnlockVirtualMemory` called after every read (Xeno.cpp lines 157, 181)
`NtUnlockVirtualMemory` is for pages explicitly locked with `NtLockVirtualMemory`.
Calling it on arbitrary un-locked pages is a no-op at best and a subtle bug at worst
(it can fail with `STATUS_NOT_LOCKED` and interfere with legitimate locks).
**Fix:** remove the `Nt.Unlock` calls from `read_mem` and `read_rbx_str`.

---

## Low

### [C# #5] `AddCheckBox` is `async void` with a post-`await` `MessageBox` (XenoUI.cpp line 405)
`MessageBox.Show` fires on the UI thread after an `await Task.Delay(200)`. If the
window is closing when the delay completes, this accesses a disposed dispatcher.
Use `Application.Current.Dispatcher.Invoke` with a null-check, or restructure to
not show a blocking dialog inside an async void.

### [C++ #10] `WaitAddr` still uses `Sleep(80)` busy-poll (Xeno.cpp line 339)
Minor inefficiency — uses `Sleep` polling rather than signalling. Not a correctness
bug but wastes CPU while waiting for a child to appear.
