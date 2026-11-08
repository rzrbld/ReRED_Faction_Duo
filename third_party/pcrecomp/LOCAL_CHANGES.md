# Local changes to vendored pcrecomp (MIT)

Source: `pcrecomp-main.zip`, sha256 in `VERSION`. Only `recomp32/` and `win32hle/` are vendored.
Every local edit is marked `[rered]` in the source.

| File | Change | Why |
|---|---|---|
| win32hle/host_lite.c | `mach_take` parks a thread whose suspend count is > 0 (`hle_suspend_pending` hook) | LS3DF.dll calls SuspendThread on running threads |
| win32hle/kernel32_crt.c | `SuspendThread`, `TerminateThread` shims | same |
| win32hle/advapi32.c, win32hle.h | `hle_reg_set_binary`, `hle_reg_has` | seed the `LS3D_setup` REG_BINARY config the original setup.exe writes |
| win32hle/host_lite.c | `HLE_TRACE=1` logs each import call with return address and first args | bring-up diagnostics |
| win32hle/module.c | DX8 DLLs (d3d8, dinput8, dpnhpast, dpnet, quartz, binkw32, vorbisfile) are pseudo-modules | LS3DF checks for DirectX 8.1 by loading dpnhpast.dll |
| win32hle/host_lite.c | per-import call counters `hle_shim_calls`, `hle_shim_count`, `hle_shim_name_at` | RERED_WATCH watchdog |
| recomp32/recomp_types.h | unresolved ICALL/ITAIL call `recomp_unresolved` (fatal, names the VA) instead of logging and continuing | a missed function must not run on into a crash |
| win32hle/dsound.c | `MAX_BUFS` 256 -> 4096 | Mafia loads hundreds of sound buffers per level; beyond 256 CreateSoundBuffer failed (DSERR_INVALIDPARAM) and LS3D crashed on the empty 3D sound |
| win32hle/dsound.c | `hle_dsound_is_playing()` | DS8 layer re-spatializes only playing 3D buffers |
| win32hle/dsound.c | released buffer objects kept (slot invalidated) in a 4096-entry ring before reuse; `hle_dsound_alive()` | LS3D calls GetStatus on buffers after their final Release (benign on Windows) |
| recomp32/recomp_types.h | `RECOMP_NONVOLATILE_MEM` option; inline fast paths for small `rep movs` (≤64 B, non-overlapping) and `MEMSET32` (≤16 dwords) | profile: ~30% of CPU in libc memmove/memset from MSVC struct copies |
| win32hle/host_lite.c | 64K direct-mapped cache in front of `recomp_lookup` | profile: 4% of CPU in the dispatch binary search |
| win32hle/hle_path.c, kernel32_ext.c | memo cache for case-insensitive component lookups (incl. not-found), invalidated by a generation counter on every create/delete/rename/mkdir/write | profile: readdir/lstat from missing-file probes |
| recomp32/recomp_types.h | indirect-call trace ring behind `RECOMP_ICALL_TRACE` (off by default) | per-call overhead on vtable-heavy code; ReThing measured it as a hot path |
| win32hle/guest_mem.c | freed large heap blocks (>32 KB) cached resident (64 entries / 64 MB) and reused | per-frame alloc/free of big buffers page-faulted fresh mmaps: ~24% CPU under memset on device |
| recomp32/recomp_types.h, win32hle/host_lite.c | x87 stack as a moving window in a 256-slot buffer (push/pop move the base instead of shifting 8 slots) | profile: 39% of game-thread CPU in the memmoves of fp_push/fp_pop |
| recomp32/recomp_types.h | `g_cur_func` bookkeeping behind `RECOMP_TRACK_CUR_FUNC` (off by default) | per-call store/save/restore in LS3D scene-graph recursion |

## ReRed Faction II ([rered])
- `win32hle/mss.c`: `AIL_set_redist_directory` and `AIL_last_error` returned host
  `static char[]` arrays (fatal "host pointer handed to the guest"); they now
  return blocks from `hle_alloc` (guest memory).
- `win32hle/bink.c`: `BinkCopyToBuffer` clips each row at the destination
  pitch (a frame drawn at an x offset wrapped into the next row), and logs its
  first three calls.
- `win32hle/user32.c`: a top-level window created with `WS_VISIBLE` now gets
  WM_ACTIVATEAPP / WM_NCACTIVATE / WM_ACTIVATE and focus, as on Windows. It was
  made active silently, so a later ShowWindow sent nothing and RF2 waited
  forever for its "application active" flag.
- `win32hle/module.c`: `SetCurrentDirectoryA` falls back to the overlay copy of
  a directory (one the game created with CreateDirectoryA, e.g. RF2's
  `profiles`), so the game can enter it; the read-only game dir lacks it.
