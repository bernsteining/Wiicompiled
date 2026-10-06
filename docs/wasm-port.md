# WebAssembly port tracker

Goal: build WiiCompiled with Emscripten and run it in a browser (Chromium first), with the same
guest-visible behaviour as the x86-64 build. Branch: `wasm-port`.

Status legend: **done** (implemented and verified), **in progress**, **todo**, **blocked**.

| # | Blocker | Status | Where |
|---|---------|--------|-------|
| 1 | [ISA layer: SIMD, FMA, NI flush, NaN rules](#1-isa-layer) | done | `runtime/include/isa/` |
| 2 | [Guest threads: stackful coroutines](#2-guest-threads) | done (backend); integration with #6/#8 | `runtime/web/host_context_jspi.cpp`, `runtime/web/host_context_jspi.js` |
| 3 | [Guest memory model](#3-guest-memory-model) | done (checked path); fast path later | `runtime/include/guest_flat_memory.h`, `memory_access.h`, `runtime/web/guest_flat_memory_wasm.cpp` |
| 4 | [Function-pointer signature casts](#4-function-pointer-casts) | done (source); link guard with #5 | `runtime/include/abi_bridge.h` |
| 5 | [Build plumbing (CMake web target, data blobs)](#5-build-plumbing) | done for tests + compile audit; product link with #7/#8 | `runtime/CMakeLists.txt`, `aurora-main/cmake/`, `translator/.../AssemblyBlobWriter.cs` |
| 6 | [Host threads](#6-host-threads) | done | AX mixer, transcript, aurora workers |
| 7 | [Graphics: aurora single-threaded on emdawnwebgpu](#7-graphics) | done for aurora's example: renders in Chromium (X11, AMD RADV) | `aurora-main/lib/`, `aurora-main/cmake/`, `aurora-main/extern/` |
| 8 | [Main loop and VI pacing](#8-main-loop) | done in code; needs the game to observe | `aurora-main/lib/aurora.cpp`, `runtime/include/platform/host_wait.h` |
| 9 | [Game files and saves](#9-game-files-and-saves) | done; verified in Chromium | `runtime/web/`, `runtime/include/hle/dvd_contract.h` |
| 10 | [Online play](#10-online-play) | todo (last) | `runtime/src/hle/net/` |
| 11 | [Distribution (`--target web`)](#11-distribution) | todo | `Launcher/` |
| 12 | [End-to-end test with a PAL disc](#12-end-to-end-test) | **plays in Chromium at 60 FPS, PS4 pad works** (user-verified) | `build-web/WiiCompiled.html` |
| 13 | [Review follow-ups: Safari/iPad, speed, cleanup, PR readiness](#13-review-follow-ups) | in progress | see the checklist |

## Environment notes

- Emscripten 6.0.8 (`~/.local/share/emsdk`, activate with `source ~/.local/share/emsdk/emsdk_env.sh`),
  Node 24.19, Chromium 153, .NET SDK 8 in `~/.dotnet`.
- Disc images: `mkwii.wbfs` is `RMCE01` (NTSC-U, unusable: WiiCompiled pins the PAL `main.dol`
  and has ~560 HLE overrides at PAL addresses). `mkwii_eu.wbfs` (repo root, git-ignored) is a clean
  PAL `RMCP01`: its `main.dol` and `StaticR.rel` match the pinned hashes. Extracted with
  `wit extract mkwii_eu.wbfs ~/mkwii/rmcp01 --psel data`; `Assets/` holds the two executables.
- Web product pipeline: `build-web-logs/translate.sh` (the `local-build.sh` translation steps with
  `--target web`), then `emcmake cmake -S runtime -B build-web -G Ninja -DCMAKE_BUILD_TYPE=Release
  -DMKW_TRANSLATED_COMPILE_JOBS=24` and `ninja -C build-web WiiCompiled`.

## 1. ISA layer

**Done.** WebAssembly gets a third backend next to x86-64 and AArch64:

- `ppc_isa_config.h`: `__wasm__` branch (Clang generic vectors, no intrinsics header).
- `ppc_isa_float.h`: `PpcPairVec` is a 2 x float generic vector. Pair arithmetic resolves NaN
  result lanes to the x86 answer (WebAssembly leaves NaN bits to the engine, and x86 vs ARM
  machines differ), like the AArch64 branch does.
- `ppc_isa_fma.h` (new): `MkwFusedMulAdd/Sub{F32,F64}`. Plain `std::fma` on x86/AArch64 (same
  codegen as before). On WebAssembly either relaxed-SIMD `madd` (default with `-mrelaxed-simd`)
  or musl's exact software `fma` (`-DMKW_WASM_SOFT_FMA`), plus x86 NaN resolution.
  `MkwHostFmaIsFused()` must be checked at startup: relaxed `madd` may legally be unfused, which
  would silently break physics parity. Safari (iOS/iPadOS, as of September 2026) rejects any
  module containing a relaxed-SIMD instruction, so Safari needs the soft build
  (`-DMKW_WEB_FMA=soft`, e.g. in `build-web-soft/`; `wasm-dis` shows no relaxed ops there vs 1802
  in the relaxed build). Safari also needs JSPI, which arrived in Safari 27 (iOS/iPadOS 27).
- `ppc_isa_fpenv.h`: WebAssembly has no MXCSR/FPCR. The "control register" is a software word, and
  the helpers replay x86 FTZ/DAZ themselves while NI is set (`PpcHostNiFlushF32/F64`,
  `PpcHostPromoteInline`, `PpcHostDemoteInline`), including x86's tininess-after-rounding rule.
  These helpers are identities on x86/AArch64.
- `ppc_isa_quantized.h`: the AArch64 branches were already portable C++; WebAssembly reuses them.

**Verification:**

- `runtime/tests/psq_helpers_tests.cpp`: 5,058,560 checks, identical checksum native vs WASM.
- `runtime/tests/fp_semantics_tests.cpp` (new): differential test of 42 scalar/paired helpers over
  edge and random inputs, NI off and on (13.4 M results). Output is per-helper hashes; WASM must
  equal native x86. Result: identical with relaxed-SIMD FMA and with software FMA, except one
  allowlisted line (below). The FMA probe passes on V8 (x86-64).

**Accepted deviations (WebAssembly vs x86-64):**

- With two or more NaN operands the result payload is the first NaN operand's. x86 itself has no
  single answer there (it depends on clang's operand order per call site); AArch64 makes the same
  choice. The test hashes such results as "some NaN".
- Under NI, plain double arithmetic the translator emits inline (`fadd`, `fmul`...) is not flushed
  on WebAssembly. x86 flushes double denormals there; real Broadway NI does not.

**Native finding (not a WASM issue):** with NI set, native x86 `PPC_Frsqrte` on a *double* denormal
returns `0x7FF0000000000001` / `0x7FFFFFFFFFFFFFFF` instead of the Dolphin estimate it returns with
NI clear. DAZ appears to reach a floating-point compare LLVM derived from integer bit tests.
Very low impact (needs NI plus a double-denormal frsqrte input).

## 2. Guest threads

**Backend done.** Emscripten's own fiber API is Asyncify-only, which would have instrumented the
whole translated game. Instead `HostContext` has a JSPI backend (
`runtime/web/host_context_jspi.cpp` + `runtime/web/host_context_jspi.js`, moved out of `runtime/src/host_context.cpp`):

- A context is first entered through the promising export `mkw_jspi_context_entry`; `Switch()` is
  the suspending import `mkw_jspi_switch`, which wakes the target from a microtask once the caller
  has suspended, then waits to be resumed.
- JSPI only switches the engine stack, so each context owns a malloc'd C shadow stack; the stack
  pointer and stack limits (including Binaryen's StackCheck copy) are swapped around each switch.
- Link flags: `-sJSPI -sJSPI_EXPORTS=mkw_jspi_context_entry --js-library .../host_context_jspi.js`.
  Node needs `--experimental-wasm-jspi`; Chrome 137+ has JSPI by default.

**Verification:** `runtime/tests/host_context_tests.cpp` and the new
`runtime/tests/host_context_stress_tests.cpp` (symmetric worker-to-worker switches, address-taken
locals and 1500-deep recursion live across switches, exceptions after resume, destroying a parked
context) pass natively (libco) and under JSPI, including with `-sASSERTIONS=2
-sSTACK_OVERFLOW_CHECK=2`, which aborts if the stack pointer ever leaves the active context's
stack. Cost: 0.17 us per switch under JSPI vs 0.009 us with libco (Node 24, Ryzen 9 9950X).

**Constraints:**

- V8 gives each JSPI stack about 984 KB of engine stack (`--wasm-stack-switching-stack-size`, not
  settable by a page). Guest recursion depth inside a fiber is bounded by that, separately from
  the 1 MiB C shadow stack.
- Remaining integration: the fiber manager's VI timer thread and the main loop (#6, #8).

## 3. Guest memory model

**Done (checked path).** WebAssembly has no fixed-address reservation, `mprotect` or SIGSEGV, and
wasm32's whole address space is 4 GiB, so there is no flat view:

- `guest_flat_memory.h`: on `__wasm__`, `RequiresCheckedAccess()` is constant-true and
  `kHasFlatView` is false. `MKW_FLAT_GUEST_BASE` expands to a trapping function, so any flat
  access that survived would fail loudly instead of writing through a null base.
- `memory_access.h`: `FlatWriteRam*` (stores the translator proved to be RAM) were the only flat
  accesses not behind `RequiresCheckedAccess()`; they now take the checked path when there is no
  flat view. Hosts with a flat view are unchanged.
- `guest_flat_memory_wasm.cpp` (new): guest RAM in ordinary allocations; MEM1's and MEM2's
  physical/cached/uncached views alias one store each, like the shared sections elsewhere.
  Protection hooks are no-ops: MMIO, deferred EFB reads and executable-write guarding are handled
  by the checked `Memory::*` path, as they already are on Apple Silicon (16 KiB pages).
- Footprint: MEM1 24 MiB + MEM2 128 MiB + Kamek overlay 2 MiB + locked cache 1 MiB.

**Verification:** `runtime/tests/wasm_guest_flat_memory_tests.cpp` (aliasing of all views,
store separation, interior/last-byte resolution, unmapped holes including MMIO, zero fill,
re-initialisation rules) passes under Node.

**Later (performance):** a masked 512 MiB window (`base + (addr & 0x1FFFFFFF)`) mirrors Wii
physical aliasing and would avoid the page-table walk, but needs inline MMIO/EFB checks on reads.
Measure first, once the game runs.

## 4. Function-pointer casts

**Done in source.** `AbiTrampoline<Ret(CpuContext*)>` fell back to
`reinterpret_cast<void (*)(CpuContext*)>(target)`, which `call_indirect` would trap on whenever
`Ret` is not `void`. The fallback is now only taken for `void` targets (no cast needed), and
`REGISTER_TRANSLATED_FUNCTION` passes `AbiRawCpuInvokerFor<&fn>()`: the function itself when it
already has the raw signature, a typed thunk otherwise. Native code is unchanged (the only user is a
`void(CpuContext*)` function). `PPC_NATIVE_OVERRIDE` already registered typed thunks, and the stored
`entryPoint` is diagnostic only.

**Still to do with #5:** the registration and dispatch-table TUs declare every symbol as
`extern "C" void f(CpuContext*)`. A native linker accepts a mismatched definition; `wasm-ld`
replaces the call with a trapping stub and only warns. The web link must use
`-Wl,--fatal-warnings` so a mismatch fails the build instead.

## 5. Build plumbing

**Done up to the product link.** Configure with Emscripten's CMake wrapper:

```sh
source ~/.local/share/emsdk/emsdk_env.sh
emcmake cmake -S runtime -B build-web -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DMKW_BUILD_PRODUCTS=OFF -DMKW_BUILD_PSQ_TESTS=ON
ninja -C build-web && ninja -C build-web mkw_web_compile && (cd build-web && ctest)
```

- `runtime/cmake/Web.cmake`, included by `runtime/CMakeLists.txt` when `MKW_PLATFORM_WEB`
  (Emscripten; the 64-bit-pointer requirement does not apply): directory-wide
  `-fwasm-exceptions -msimd128` (+ `-mrelaxed-simd`, or `-DMKW_WASM_SOFT_FMA` with
  `-DMKW_WEB_FMA=soft`), `-sALLOW_MEMORY_GROWTH=1`, static third-party libraries;
  `mkw_web_link_jspi()`; `mkw_web_select_sources()` (drops the flat-memory backends, Discord IPC and
  the native `host_context.cpp`, adds `runtime/web/*.cpp`); `mkw_web_add_tests()`;
  `mkw_web_add_compile_audit()`. `runtime/cmake/WebProduct.cmake`: the product link
  (`mkw_configure_web_product`, `MKW_WEB_LINK_OPT`, `MKW_WEB_PROFILING`). Every web-only source is
  in `runtime/web/`.
- `aurora-main/cmake`: under Emscripten, Dawn comes from the `emdawnwebgpu` port and SDL3 from the
  `sdl3` port; `WEBGPU_DAWN` is not defined (`AURORA_WEBGPU_BROWSER` instead) and the native
  surface binding is not compiled. abseil, xxhash, fmt, zlib, libpng, Freetype, imgui, sqlite, zstd,
  Tracy and mbedTLS build from source unchanged.
- `mkw_web_compile` (with `MKW_BUILD_PRODUCTS=OFF`): compile-only audit of every runtime source for
  WebAssembly against the real dependencies, like the existing macOS audit. Clean.
- Translator: `--target web` (global CLI option) makes the data-blob assembly WebAssembly-shaped
  (one named section per symbol, `.type`/`.size`, no Mach-O underscore, no ELF stack note). Native
  output is unchanged and still follows the host OS.

**Verification:**

- Web `ctest` under Node: 12/12 pass (FP semantics vs x86 reference, PSQ helpers and reserved
  encodings, JSPI contexts, guest memory, runtime config, NAND save/settings, SC serial, input
  expressions, platform paths).
- Native Linux `ctest` (same tree): 9/10 pass. `mkw_platform_paths_tests` fails, and already failed
  before this branch: `RuntimePlatform::ExecutableDirectory()` has no Linux implementation.
  (Configuring natively here needs `-DSDL_X11_XSCRNSAVER=OFF`, because this machine lacks the
  XScrnSaver headers for SDL's vendored build.)
- Translator: 646/646 (`dotnet test translator/Translator.sln -c Release`), including new
  `AssemblyBlobWriterTests`. The WebAssembly blob form was also assembled and linked by hand with
  em++: byte-identical to the native build.

**Fixes found by the web tests:** `nand_settings.h` published `setting.txt` with `link()`, which
Emscripten's file systems do not support, so NAND initialisation would have failed in the browser
(now `rename()` after an existence check on web). `network_ssl.cpp` was missing `<sys/time.h>`.
`runtime_config.h`/`main.cpp` had no graphics-backend table for the web.

The product link (`WebProduct.cmake`: JSPI flags, `-Wl,--fatal-warnings` for #4, page shell,
asset packaging) is done; see #12.

## 6. Host threads

**Done.** The web build is single-threaded (no SharedArrayBuffer, so no COOP/COEP headers needed):

- AX/DSP mixer: `EnsureMixWorker()` returns false on web, so command lists mix inline (the
  existing fallback path).
- Process transcript (stdout/stderr pumped into `console.log` by two threads): skipped on web;
  the browser console already records it.
- Deferred DNS worker: already fails DNS cleanly when it cannot start a thread (online is #10).
- Music ducking monitors: no branch for the web, so no thread is started.
- aurora: frame worker, presenter, pipeline compile workers, pipeline-cache writer and
  interpolation helpers are all off on web (see #7).

## 7. Graphics

**Renders in Chromium** (aurora's own example; the game itself needs a PAL image).

- `gpu.cpp`: canvas surface (`#canvas`) instead of the native window binding; the browser picks
  the backend; Dawn-native features (MetalFX IOSurface, implicit device sync, logging callback)
  stay behind `WEBGPU_DAWN`. Timed `WaitAny` works through JSPI.
- `aurora.cpp`: no frame worker, presentation synchronous (like Apple), no `Surface::Present()`
  (it aborts in the browser) and no busy-wait pacing; instead `end_frame` suspends until the next
  animation frame (`aurora_web_wait_for_browser_frame`), which is when the browser shows the
  canvas and delivers input.
- `pipeline_cache.cpp`: pipelines were already built synchronously for `BackendType::WebGPU`; the
  cache writer thread is replaced by an inline flush at frame end.
- Frame interpolation is off on web (`frame_interpolation_fps` only accepts 0 there).
- `extern/CMakeLists.txt`: fmt needs `<cstdlib>` with libc++ (as on MinGW); imgui 1.91.9b's WebGPU
  backend predates emdawnwebgpu, so a generated copy with only its Emscripten guard removed is
  compiled for the web (the upstream sources are untouched).
- `dds_io.cpp`: an explicit narrowing for 32-bit `size_t`.
- Two waits that can only finish once the page returns to the browser: the staging-buffer map
  wait in `gfx::begin_frame` (`staging_map.hpp`) suspended through JSPI instead of spinning (since
  replaced: the browser build no longer maps staging buffers, see P12 in #13),
  and `aurora_update()` yields when no `end_frame` has since the last call, so a loop that never
  completes a frame (paused, surface not ready) cannot freeze the page.

**Verification:** all aurora libraries build for WebAssembly and still build natively.
`mkw_web_aurora_smoke` (aurora's own example, which clears the screen through GX) renders its
dark blue clear colour through the whole pipeline (EFB clear, XFB copy pipeline, canvas) in a
windowed Chromium 153 on the X11 session, AMD iGPU via Mesa RADV (`mesa-vulkan-radeon`). The page
stays responsive (DevTools probes and screenshots answer while it runs).

```sh
python3 runtime/web/serve.py build-web --port 8000
chromium --enable-unsafe-webgpu --ozone-platform=x11 --enable-features=Vulkan \
    http://127.0.0.1:8000/mkw_web_aurora_smoke.html
```

Notes for this machine: Chromium needs a Vulkan driver for WebGPU (`mesa-vulkan-radeon`). Do
**not** add `--use-angle=vulkan` / `VulkanFromANGLE`: ANGLE's Vulkan backend fails to initialise
here (`VK_ERROR_INITIALIZATION_FAILED`) and takes the whole GPU process down; the default ANGLE GL
compositor works. Headless Chromium has no working GPU process here in any mode, so rendering
checks need the X session (`DISPLAY=:0`).

## 8. Main loop

**Done in code.** The game's loop keeps its native shape; it just never blocks the page:

- aurora yields to the browser once per frame (#7).
- `runtime/include/platform/host_wait.h` (new): `HostWait::SleepFor/SleepUntil` block natively
  (unchanged behaviour) and suspend through JSPI (`emscripten_sleep`) on web. Used by VI pacing
  (`SleepPreciselyUntil`, which also skips its spin-wait on web), the begin-frame retry loop, the
  Bluetooth `GKI_delay` HLE and the panic flush delay. SDL3's own `SDL_Delay` already suspends
  under JSPI.
- Suspension works from inside guest fibers too: each fiber suspends on its own JSPI stack.

Needs the game to observe pacing and responsiveness. Browser timers are millisecond-grained.

## 9. Game files and saves

**Done; verified in Chromium.**

- `runtime/web/disc_fs.js` (new): a read-only Emscripten file system mounted at `/dvd`
  (the web default for `paths.dvd_root`). Directory listings and sizes come from the page's file
  list, so the DVD HLE's `std::filesystem` discovery runs unchanged. Bulk data never goes through
  it: `DvdReadContract::ReadExact` (the single choke point for disc reads) calls
  `mkw_web_disc_read`, which suspends through JSPI while the page reads the byte range. Only
  `sys/*` (read with `std::ifstream`) is fetched up front.
- `runtime/web/loader.js` (new, `--pre-js`): before `main()`, takes the game's files from, in
  order: the copy installed in this browser (#13 S11); with `?data=<url>`, a server; or what the
  player picks on the setup panel: their disc image (#13 S10) or an extracted DATA folder
  (`showDirectoryPicker`, or an `<input webkitdirectory>` where that is missing), read in place,
  never uploaded. Only `RMCP01` is accepted; mounts `/userdata` on IndexedDB (IDBFS) for Config.toml, the NAND
  and saves, written back every 5 s and when the page is hidden (one sync at a time), after running
  `Module.mkwUserDataLoaded` hooks. Disc reads over HTTP go through a cache of 256 KiB blocks
  (64 MiB, least recently used out; consecutive missing blocks in one request, in-flight blocks
  shared) and retry dropped connections three times.
- `runtime/web/unlock.js` (new, `--pre-js`): `?unlock`, below.
- `runtime/web/shell.html` (new): the page (canvas, status, folder button), the prerequisite check
  (secure origin, WebAssembly, WebGPU, JSPI), download/compile progress of the game code, fatal
  errors shown over the game (`Module.mkwShowError`, also called by the runtime's fatal path), and
  with `?log` page output, errors and startup steps posted to the server.
- `runtime/web/serve.py` (new): server with byte-range support and an optional
  `--data` directory exposed with a generated manifest. Localhost-only by default; `--host 0.0.0.0`
  serves your own network, which needs HTTPS (`--cert/--key`) because browsers only enable WebGPU
  on secure origins. `runtime/web/make_cert.sh <dir> <lan-ip>` makes a local certificate authority
  (`ca.pem`, reused on later runs) and a server certificate it signs (`server.pem`,
  `server-key.pem`) that follows Apple's TLS rules (subjectAltName, serverAuth, not a CA, under 398
  days). Most browsers accept it after one warning; with `--ca ca.pem`, devices that do not let you
  past the warning (an iPhone did not) can install and trust the authority from
  `http://<lan-ip>:<port>/ca.crt`, which the plain-http page links to with the steps.
  TLS handshakes run on each connection's thread with a 10 s timeout: the first version did them
  in `accept()`, and one stalled client (a preconnect, a device rejecting the certificate) froze
  the server for everyone. Plain http:// on the TLS port gets a page linking the https:// address:
  Safari tries https:// for an address typed without a scheme, falls back to http:// when the
  self-signed certificate is refused, and would otherwise just fail. Without TLS, another computer can use an SSH tunnel to the
  localhost-only server (`ssh -N -L 8000:127.0.0.1:8000 <host>`, then `http://localhost:8000/...`),
  since localhost counts as a secure origin. HTTP/1.1 keep-alive (one TLS handshake per connection),
  Range replies streamed in 1 MiB chunks, `.wasm`/`.data`/`.js` sent gzip-compressed when the
  browser accepts it (cached as `<file>.gz`, remade after a rebuild; the wasm goes from 117 to
  26 MB), and `--remote-log` prints what pages send with `?log` (an iPad has no reachable console).
- The page names a missing prerequisite (secure origin, WebGPU, JSPI) instead of failing later.
- `/` redirects to the game page (with `?data=data` when serving data), so on a phone or tablet
  the address to type is just `host:port`.
- `?unlock` (or `serve.py --unlock-all`, which adds it to the `/` redirect): before the game reads
  its save, `unlock.js` sets unlock bytes 0x30-0x37 of every license in
  `/userdata/*/NAND/title/00010004/524d4350/data/rksys.dat` (all cups, characters, vehicles,
  Mirror) and rewrites the big-endian CRC-32 at 0x27FFC; the first time, it keeps the previous
  save as `/userdata/<app>/rksys-before-unlock.dat`. A browser without a save must create a
  license first and reload. Verified in a throwaway headless Chromium (loader smoke page, real
  save planted in IDBFS): only those 8 bytes and the CRC change, and the result persists; the game
  accepting it is to be confirmed on screen.
- Web paths: runtime assets under `/app` (`ExecutableDirectory()`), user data under
  `/userdata/WiiCompiled`.

**Verification:**

- `mkw_web_disc_fs_tests` (Node): discovery and sizes, JSPI reads byte-for-byte, the native
  error semantics (missing file, bad offset, short read), sync read of up-front files, async-only
  files failing sync reads cleanly, read-only mount.
- `mkw_web_loader_smoke` in headless Chromium 153 through `serve.py --data` on a synthetic DATA
  tree: every file's hash matches an independent Python hash (including an empty file and a name
  with a space), 14 range requests, and a `/userdata` counter went 1 then 2 across page loads.
- Not automatable here: the folder picker itself (needs a user gesture).

## 10. Online play

Todo, last. Needs a WebSocket relay and WebRTC.

## 11. Distribution

Todo. Translated code must never be hosted publicly, so the web build is produced locally and
served from localhost (`serve.py`). Remaining: the product link in `PublicProducts.cmake` (JSPI,
`disc_fs.js`, `loader.js`, `shell.html`, `-lidbfs.js`, `-Wl,--fatal-warnings`, assets preloaded
under `/app`), and a Setup option that runs the translator with `--target web` and builds it.

## 12. End-to-end test

**Mario Kart Wii boots and plays in Chromium** (2026-10-04, clean PAL `RMCP01`, windowed Chromium
153 on X11, AMD iGPU via RADV), at 60 FPS, with a PS4 controller (DS4 v2), as verified by the user
playing it. The product links with `-Wl,--fatal-warnings` (no signature mismatches);
`WiiCompiled.wasm` is 136 MB with function names kept.

Fixes this needed beyond #1-#9:

- `aurora_update()` waited for an animation frame whenever no frame had ended since its last call;
  the runtime calls it on every VI retrace, which cut the game to ~6 FPS (the main thread was 95%
  idle). It now yields one immediate event-loop turn, and only if the page has not yielded for 8 ms.
- The staging-buffer map wait in `gfx::begin_frame` suspends through JSPI (#7; later removed, P12).
- Shell overlay CSS (`display: flex` overrode `hidden`).
- Starting a race hung on the loading screen: the race scene sets a GX viewport depth range of
  0.95..1.06, which the browser's WebGPU validation rejects (native Dawn runs with validation
  skipped). aurora's fatal-error handler aborted inside the browser callback while the game was
  suspended, so the page froze instead of crashing visibly. Fixes: the web build clamps viewport
  depth to [0,1] after the reversed-Z remap (`lib/gfx/common.cpp`; a range past 1 is compressed,
  which can shift depth compares slightly for what uses it), and the shell shows aborts on the page.
  Expect more of this class: any GX state the native build only gets away with because validation
  is off will abort in the browser.

Gamepads need no web-specific code: SDL3's Emscripten backend reads the browser Gamepad API. The
browser only reveals a controller after a button press while the page has focus.

Seen in the console, not yet investigated: `audio output queue full; dropping blocks` (SDL3's
browser audio still uses the deprecated ScriptProcessorNode), and the first boot spends ~28 s
prewarming 1199 pipelines because pipelines compile synchronously on the web.

Note for testing: DevTools `Page.captureScreenshot` can return stale frames of the WebGPU canvas;
capture the real window with `xwd -id <window>` instead.

## 13. Review follow-ups

Two read-only reviews (2026-10-04) of the port: one classified every hunk in the 37 pre-existing
files (76 web-only, 84 compiled by native but identical by construction, 4 that can differ, none
that must stay non-additive), the other looked for speed, cleanup and robustness issues in the new
code. Plus the iPad (Safari 27) bring-up. Status: todo / in progress / done (verified how).

| ID | Item | Status |
|----|------|--------|
| R1 | iPad Pro 11" (M2), iPadOS 27.2, Safari 27.2: page stopped at "Game data ready" | **boots** (2026-10-05). Three causes, found with `?log` (`serve.py --remote-log`): (1) IDBFS's load of `/userdata` never answered: a frozen earlier copy of the page held a write transaction on the store, so even `count()` waited; closing Safari fixed it, and the loader now starts without saves after 8 s with a notice instead of hanging (verified in Chromium with a tab holding the lock). (2) Safari rejects `array<u32, 12>` in a uniform buffer (WGSL needs a 16-byte stride without the `uniform_buffer_standard_layout` extension, which Chrome has): the browser build declares `array_start` as `array<vec4u, 3>` (same 48 bytes) in `lib/gx/shader.cpp`. (3) the relaxed-SIMD build cannot load (soft build). Open: the 1199-pipeline prewarm compiles 5 pipelines per frame on the page thread (~14 ms each in Safari), so the first ~20-30 s run slowly; next step is `CreateRenderPipelineAsync` on the web |
| P1 | `serve.py`: HTTP/1.1 keep-alive (one TLS handshake per connection, not per read) | done: `protocol_version` HTTP/1.1, every reply has a length; curl reuses one connection for two ranges |
| P2 | Loader: aligned block cache and in-flight dedupe for disc reads | done: 256 KiB blocks, 64 MiB LRU, runs of missing blocks in one request, shared in-flight blocks (`loader.js`) |
| P3 | Zero-length waits use the MessageChannel yield, not `emscripten_sleep(0)` (~4 ms timer clamp) | done: `HostWait` keeps whole-ms timers plus turns for the remainder, both yield helpers reuse one MessageChannel. (An immediate turn in aurora's staging-map wait had starved the browser; that wait no longer exists, see P12) |
| P4 | Web frame pacing: `requestAnimationFrame` as the only pacer (no 60 -> 30 FPS cliff) | reverted (measured regression): returning early when a display frame had already passed (timer or MessageChannel instead of requestAnimationFrame) starved Chrome's rendering, 60 -> ~2.4 FPS with the page 97% idle. `aurora_web_wait_for_browser_frame` always waits for the animation frame again (60 FPS, 181 display frames in 3 s) |
| P5 | Soft FMA: exact fast path when the product is exact in double | done: `MkwRawFmaF64` (soft) returns `a*b + c` when a zero factor or the significand/exponent test proves the product exact; soft-build `mkw_fp_semantics_tests` passes |
| P6 | Guest memory: direct MEM1/MEM2 path on wasm (MMIO and write policy still checked) | closed, not worth it: on wasm a guest read is already one 16 KiB bias-table load plus an add (`TryReadGuestScalar`), a write adds two range checks. That table is what honours deferred-read and executable-write pages, which native builds get from page protection and wasm cannot; a table-free path would save one cached load per access, not 10-30% |
| P7 | Release link flags (`-O2`, no name section) and a precompressed `.wasm` | done: link level `MKW_WEB_LINK_OPT` (default `-O3`; `MKW_WEB_PROFILING=ON` gives the old `-O1 --profiling-funcs`); `serve.py` sends gzip copies of .wasm/.data/.js. Measured on the relaxed build (link time, wasm, gzip): -O2 19 s 117.0/26.5 MB, -O3 32 s 116.6/26.3 MB, -Os 30 s 116.1/26.3 MB, -Oz 32 s 108.6/25.6 MB (was 136 MB at -O1 with names) |
| P8 | Paired-single FTZ result check as one vector compare | done: one vector compare against FLT_MIN before the per-lane exact check |
| C1 | Call `MkwHostFmaIsFused()` at startup; refuse to run unfused (relaxed build on a CPU without FMA) | done: `RuntimeMain` throws when `MkwHostFmaIsFused()` is false in a relaxed build; web fatal errors now show on the page (`mkw_web_show_fatal`) |
| C2 | One x86 NaN rule for scalar and pair; fused multiply-subtract resolves NaNs once | done: raw backend helpers `MkwRawFma*`, each fused helper resolves NaNs once; the 2-operand pair resolver reuses the 3-operand one |
| C3 | One "give the browser a turn" primitive (aurora yield, staging map, `HostWait`) | done with P3: aurora keeps its one turn primitive, the runtime has `mkw_web_yield_to_browser` |
| C4 | JSPI contexts: drop the C++ id map that mirrors the JS map | done: contexts are named by their `Context*`; no C++ map |
| C5 | One `/dvd` constant; save path cross-referenced with `nand_path.h` | done: `platform/web_mounts.h` (`/dvd`, `/userdata`, `/app`); `unlock.js` cites `nand_path.h` |
| C6 | `?unlock` in its own `--pre-js`, out of the generic loader | done: `unlock.js` (`--pre-js`) hooks `Module.mkwUserDataLoaded`, which the loader runs after loading /userdata |
| C7 | Stale comments: `ppc_isa_wasm_fma.h`, "~155 MiB", "verified at startup" | done |
| S1 | Disc reads retry with backoff (a Wi-Fi blip showed the game's disc error) | done: network errors retried 3 times (0.5, 1, 2 s); HTTP error answers are not |
| S2 | Page checks: `typeof WebAssembly` guard (Lockdown Mode), relaxed-SIMD probe that names the problem | done: `typeof WebAssembly` check; relaxed-SIMD `WebAssembly.validate` probe names the problem when a compile fails |
| S3 | IDBFS `syncfs` in-flight guard | done: one sync in flight, a request during it schedules one more |
| S4 | `serve.py` streams Range replies instead of reading them into memory | done: 1 MiB chunks; a full-range read of the wasm compared byte for byte |
| A1 | Move the wasm FP code into `ppc_isa_float_wasm.h`; keep one-line seams in `ppc_isa_float.h` | done: `ppc_isa_float_wasm.h` holds the wasm host-environment helpers and paired arithmetic; `ppc_isa_float.h` keeps the per-function `#elif __wasm__` branches and identity helpers for other hosts |
| A2 | Move the JSPI backend into `host_context_jspi.cpp` | done: `runtime/web/host_context_jspi.cpp`; `runtime/src/host_context.cpp` is back to its committed content and excluded on web |
| A3 | Move the web CMake blocks into `cmake/Web.cmake` / `cmake/WebProduct.cmake` | done: `cmake/Web.cmake` (options, paths, `mkw_web_select_sources`, `mkw_web_add_tests`, `mkw_web_add_compile_audit`) and `cmake/WebProduct.cmake` (`mkw_configure_web_product`); `runtime/CMakeLists.txt` went from +180 to +76 lines, `PublicProducts.cmake` keeps only the call |
| A4 | Move the web sources out of `src/` (drops the native GLOB exclusion) | done: `runtime/web/` holds every web-only source (C++, JS, HTML, `serve.py`); the native source GLOB needs no exclusion |
| A5 | Guard the small native-visible leftovers (`memory_access.h`, `common.cpp` const, imgui include dir, CMake message) | done: imgui include dir under `if (EMSCRIPTEN)`; `common.cpp` keeps its `const` lines (the web clamp is a guarded block); `memory_access.h` routes only `FlatStore` on wasm instead of five `FlatWriteRam*` lines; the CMake error message is back to its committed text. `abi_bridge.h` stays: it fixes undefined behaviour on every host (separate PR) |
| A6 | Native `mkw_fp_semantics_tests` behind an option | done: `MKW_BUILD_FP_SEMANTICS_TESTS` (OFF) for x86-64; always built for the web |
| A7 | Prove native builds unchanged: compare native objects before/after | done for the translated code: all 89 base shards (common, dispatch, registration) compiled natively with the Linux x86-64 product flags (`-march=x86-64-v3`, `-O2 -fno-fast-math -ffp-contract=off -fno-slp-vectorize`) against the committed headers (HEAD worktree) and the working tree, `-ffile-prefix-map` normalising paths: 89/89 objects byte-identical. Runtime and aurora sources (HostWait call sites, pipeline-cache refactor) are identical by construction but not yet compared: that needs a native product build with Dawn |

| P9 | Asynchronous pipeline compiles in the browser (`createRenderPipelineAsync`): in-frame GX pipelines queue like the native worker pool, draws skip unready ones, a pass that must not skip compiles synchronously; completions via `ProcessEvents` | done: desktop prewarm 251.8 s -> 6.2 s (cold Chrome shader cache 41.9 s), iPad 17 s -> 3.0 s |
| S5 | Safari: "encoder state is not valid" / "encoder state is 'Locked'", then a failed staging-buffer map | done, verified on the iPad (2026-10-05: menus, two scene changes and a Luigi Circuit race with 0 validation errors). Two causes, both invalid only in WebKit or only at an iPad's 2x pixel ratio: (1) `tex_copy_conv` conversion pipelines were RGBA8-only while copy targets use the surface format, BGRA8 in Safari: BGRA8 variants in the browser build; (2) GX viewports larger than the render target (5760x1870 on a 3840x1874 target at 2x), which Chrome accepts and WebKit rejects, leaving the encoder locked: the browser build sets the viewport clamped to the target and folds the compensation into the projection (`browser_viewport_clamp`, `effective_projection`; random test: <= 0.002 px off). Kept: validation errors logged (first 32) instead of fatal, and copy checks (`lib/webgpu/browser_checks.hpp`) that name a copy breaking WebGPU's rules |
| S6 | Stale cache: Safari reused a cached `.wasm` next to a newer script and crashed at startup | done: `serve.py` sends `Cache-Control: no-cache` for build files (revalidation, 304 when unchanged) |
| S7 | Safari: a reloaded or left copy of the page, frozen in the back/forward cache with an IndexedDB transaction open, blocks the next copy's `/userdata` load | done: an `unload` handler keeps the page out of the back/forward cache (frozen copies from before it remain until Safari is closed); the 8 s fallback stays |
| P10 | `?lowres` (on by default in `serve.py --lowres`): 1x canvas (`devicePixelRatio` replaced) and `resolution_multiplier = 1.0` written to Config.toml before start | done: iPad reports `framebuffer size 640x528 scale 1`, canvas 1194x775 instead of 2388x1550 |
| E1 | Desktop Chromium's GPU process switched WebGPU to the RTX 5090 (display on the AMD iGPU, so every frame crosses GPUs: ~2-3 FPS) and later lost the device on the low-power adapter | environment, not code: restart that Chromium. aurora asks for `HighPerformance`; a page option for the low-power adapter would suit machines whose display GPU is the integrated one |
| P11 | iPad speed: `?log` perf report every 5 s (game/display fps, busy %, ms per frame, waits: frame, yield, sleep, disc; thread switches and their cost; CPU ms per frame in GX decode, frame encode, audio) | measured 2026-10-05 on the iPad (soft build, 640x528, 1x canvas): intro 57-60 fps at 13-14 ms of work per frame; character select 17-19 fps at 47-53 ms; Luigi Circuit ~32 fps at 25-27 ms, busy ~85%, ~1.6 ms/frame waiting for the staging map. JSPI thread switches are NOT the cost: 260-370/s at 4-18 us each (~10 ms per second). The time goes into running code (game, GX decode, encoding); next: split that into buckets |
| S8 | Safari: entering Time Trials, a staging-buffer map fails (`map async was not successful`, no validation error) and the next map never completes | to retest: WebKit fails a map when the previously submitted GPU work ended in error (Metal command buffer failure), so some Time Trials GPU work faulted on the iPad. The browser build no longer maps staging buffers at all (P12), so that failure can no longer stall the frame loop; whether the GPU fault itself remains, and what causes it, needs a run on the iPad |
| P12 | Staging uploads without mapped buffers in the browser | done (2026-10-05): a CPU profile of a race in desktop Chrome showed 33% of the main thread in garbage collection and 28% in browser internals. emdawnwebgpu cannot hand wasm the mapped memory: `GetMappedRange` allocates a heap copy of each range and `Unmap` copies every range back, 37 MiB per frame whatever the frame used, plus a round trip to the GPU process per map and detached ArrayBuffers for the collector. The browser build now writes a frame's data into one block of wasm memory and uploads only the used bytes with `queue.WriteBuffer` (`lib/gfx/common.cpp`); no staging buffers, maps or map waits remain (the 1 s stuck-map fallback and `aurora_web_staging_wait` are gone). Desktop race: encode 2.0 -> 0.6 ms per frame |
| P13 | Retrace paced by the display refresh in the browser | done (2026-10-05): the guest waits for each VI retrace on a wall-clock grid in the scheduler's idle loop, then aurora waits for `requestAnimationFrame`: two unsynchronised 60 Hz clocks, so whenever their phases drift apart a frame that fits in one refresh misses it (desktop race after P12: 36-46 fps with ~11 ms of work per frame). Now, while the display refreshes within 10% of the retrace rate, each refresh is the retrace: a refresh ticker (`mkw_web_display_tick`, `runtime/web/host_wait_web.cpp`) drives `AdvanceDueRetraces`, the idle wait and the paced present (`src/hle/vi.cpp`), and aurora skips its own wait when a refresh has passed since the previous frame (`Module.mkwDisplay.locked`). Each frame still waits for exactly one refresh (P4's lesson). Other rates (120/144 Hz, PAL50), hidden pages and stalls over 100 ms keep the wall-clock grid. Desktop race: 59-60 game fps = display fps, no sleep, 10-12 ms of work per frame. `?novsync` (or `/?novsync`: `serve.py` passes options given to `/` on to the game page) turns it off for comparison. First iPad run with P12+P13 (2026-10-05): the page froze right after VIInit (event loop stalled 22 s, then 15 s twice, then `createCommandEncoder` rejected) and the reload hung while compiling the wasm, so Safari itself was wedged (the previous session, on the older build, had also ended frozen); retested after quitting Safari: the freeze was the cold pipeline prewarm (P16), not the pacing. **iPad verified 2026-10-05** (default, pacing on): Luigi Circuit race at 59-60 game fps = display fps, 9-13 ms of work per frame (was ~34 fps at 23-26 ms before P12+P13); the user reports it playable with occasional drops. Also: `EM_ASYNC_JS` imports are named `__asyncjs__<name>`, so `host_wait.h` declares them with `EM_IMPORT` (a call compiled before the definition, as in a unity build, otherwise links to the plain name and fails) |
| P14 | Can Safari use the relaxed-SIMD build? | no: the page's probe on the iPad (Safari 27.2) reports `relaxed SIMD not supported by this browser`; WebKit implements it behind a flag only. The soft build stays for Safari; on desktop Chrome the soft build costs ~0.5 ms per frame more than the relaxed one |
| P15 | Does JavaScriptCore leave our big functions unoptimized? | not the bottleneck: JSC never tiers a wasm function over `maximumOMGCandidateCost` (100,000 bytes on WebKit main) up to OMG. 29 functions are over it at `-O3`, and the functions over it take ~3% of wasm self time in a race profile (31% for those over 16 KB) |
| S9 | iPhone: the page never loads, even after accepting the certificate warning | done, to verify on the iPhone (2026-10-05): every TLS handshake from the iPhone ended with `certificate unknown` and Safari fell back to plain http. The certificate was a bare self-signed one marked as a CA, without the serverAuth usage Apple's TLS rules ask for; the iPad let it through, the iPhone did not. Now `make_cert.sh` issues a compliant server certificate from a local CA, and `serve.py --ca` offers that CA at `http://<address>:<port>/ca.crt` for the device to install and trust (verified with curl against the CA: chain and address match) |
| P16 | Safari: cold pipeline compiles freeze the display | todo: the first start after quitting Safari spent 147.7 s on the 1548-pipeline prewarm with the page's display refresh at 0.1-0.4 per second (the screen looks frozen); with Safari's compiled pipelines still in memory the prewarm takes ~3 s. Up to 64 asynchronous compiles in flight seem to queue ahead of Safari's rendering in its GPU process, and each frame of the prewarm waits behind them. The occasional drops in a race (display fps falling with the game fps, CPU mostly idle) look like the same thing for pipelines compiled on demand. Next: far fewer compiles in flight on WebKit, and measure. Also seen: Safari reopened several game tabs at once after the restart, and one stopped in aurora's start-up with `std::terminate() without current exception` |
| S10 | Game data from the player instead of the server, in every browser | done (2026-10-05): the setup panel's "Open disc image" is a plain file input (works on iPadOS, Safari, Firefox, Chrome). `runtime/web/disc_image/`: nod at the tag the native setup uses (v2.0.0-alpha.10, so ISO, WBFS, RVZ, WIA, CISO, GCZ) behind a small C ABI (`src/lib.rs`), built with cargo for wasm32-unknown-emscripten (its C compressors compile against Emscripten's libc) and linked into `mkw_disc.{js,wasm}` (260 KB gzipped) by `cmake/WebProduct.cmake` (`MKW_WEB_DISC_IMAGES`, needs cargo and `rustup target add wasm32-unknown-emscripten`). `disc_worker.js` runs it in a Web Worker that reads the picked File in place with FileReaderSync and decrypts there; no Nintendo key or code is in this repository (nod brings them, as for the native setup). Verified: all 2032 files and the 5 `sys/` files read from `mkwii_eu.wbfs` are byte-identical to the extracted folder (Node, 2.68 GB at 211 MB/s); desktop Chrome boots the game from the picked WBFS (opened in 0.1 s), and from an extracted folder through the `webkitdirectory` fallback. `serve.py`: `/?pick` opens the page without the served data |
| S11 | Install once: the game's files kept in the browser | done (2026-10-05): with "Install in this browser" ticked (or `?install`, which also installs from a `?data` server) the files are copied into the Origin Private File System, flat (`f0`, `f1`, ...) with `manifest.json` written last (no manifest = interrupted install, ignored), through a worker using synchronous access handles (the OPFS write API Safari has too). Storage is checked against `navigator.storage.estimate()` and `persist()` is requested. Later visits start from it before anything else; `?uninstall` removes it. Desktop Chrome: 2.7 GB installed in 35 s from the WBFS, reload started from it (0 ms of disc waiting). Not yet tried on the iPad (iPadOS may copy a picked 2.8 GB file before the page gets it) |
| P17 | Logging what the game spends its frames on | done (2026-10-05), with `?log`, every 5 s: frame time distribution (worst, over 20/33/50 ms) and display refresh gaps; retraces paced by the display vs the clock; per frame: draws, draws skipped for an unready pipeline, upload KB, texture uploads, staging batches; pipelines started/finished (average latency), compiled synchronously (ms), still compiling; disc reads, MB, worst latency; audio blocks dropped and queue run dry; wasm memory (and Chrome's JS heap with buffers); long tasks where the browser reports them. Every frame over 50 ms gets a `hitch:` line with that frame's waits, CPU buckets, pipeline, texture and disc activity. Counters: `lib/web_perf.hpp` (aurora), `platform/web_perf.h` (runtime). First finding: in desktop Chrome, starting 32 asynchronous pipeline compiles costs ~140 ms of main-thread time (shader generation and creation calls), so the prewarm frames take 120-190 ms; this is a lead for P16 |
| P18 | iPad session with the P17 logs (2026-10-05, warm Safari: prewarm 1599 pipelines in 2.9 s) | findings: (1) race at 58-60 fps, 8-11 ms of work per frame (gx decode ~1 ms, encode 0.4, audio 0.4: the rest is the game's own code); the user calls it playable with drops at specific moments. (2) Those drops are new pipelines: a 978 ms frame (a MessageChannel turn that took 960 ms while 5 pipelines compiled, average latency 978 ms) and a 503 ms one (2 started); Safari stalls the whole page while it compiles, asynchronous or not (P16). (3) The display pacing (P13) mostly fell back to the clock: the refresh period was the shortest interval, and ProMotion's 120 Hz timestamps give some 8.3 ms intervals; now the median. (4) Every audio block was dropped (~1670 per 5 s): the output queue never drains; the report now shows the AudioContext state and rate. To check: whether the iPad plays sound |
| P19 | Smaller generated shaders in the browser | done (2026-10-05): each GX shader carried the same ~12 KB prelude of helpers (vertex fetch for every format, TEV arithmetic) for ~4 KB of its own code; in Chrome 1993 modules (16 KB median, 32.3 MB in total) cost 15.7 s in `createShaderModule`, and a profile of the prewarm was 19% `createShaderModule` and 50% garbage collection (shader generation itself: ~0%). `lib/gx/wgsl_prune.hpp` drops the functions a shader never calls before the module is created (browser build only): 18.2 MB (-44%), 9.5 s (-39%), no validation errors; a sample of pruned shaders also compiled cleanly on their own. Expected to help Safari more, which translates and compiles every shader. Answer to "are shaders compiled in parallel": the page side (generate, create the module, start the pipeline) is sequential on the page's only thread; Chrome compiles the started pipelines in parallel in its GPU process; Safari stalls the page instead |
| P20 | A fuller pipeline seed for the web build | done (2026-10-05): `runtime/assets/pipeline/web_pipeline_cache.db` = the native seed (1199) + 1550 configurations desktop Chrome learned in play = 2749 (8.3 MB, 210 KB gzipped), preloaded as `/initial_pipeline_cache.db` by the web build only (native keeps `initial_pipeline_cache.db`). Merged with `runtime/web/pipeline_seed.py merge <seed> <learned.db>...` (uv script: matches rows by configuration bytes, recomputes the native 64-bit XXH3 key that 32-bit web builds truncate, checks its hashing against every existing row). The seed is merged into each device's cache at every start, so devices with a cache get it too. Verified: 2749 rows merged, none skipped, prewarm 14.3 s in desktop Chrome. Trade-off: a cold Safari start compiles more; ubershaders (P22) are the real fix. To grow it later: export a device's `Cache/pipeline_cache.db` (+ `-wal`) and merge |
| P21 | Where the game's own code spends its time (~8 ms of the iPad's frame) | measured (2026-10-05). Two guesses ruled out by reading the code: the translator already keeps guest registers in C++ locals per function, and guest loads/stores are inline fast paths (one page-table load, a byte swap, two range checks for writes), not calls. A measurement build (`-DMKW_MEASURE_HELPERS -fno-inline-functions`: `MKW_MEMORY_FORCE_INLINE`/`MKW_PPC_FORCE_INLINE` become out-of-line so a profile names them; their call overhead inflates their share) profiled on a race in Chrome, share of busy CPU: guest memory access 27.8% (`Memory::Read32` 8.0%, 32-bit writes 5.7%), translated logic 19.3%, aurora GX decode/draw 12.3%, indirect dispatch 3.7%, paired-single load/store 3.1%, FP exactness (NaN, flush, FMA) 2.8%, `performance.now` 2.4% + wasm-to-JS 1.3%. That last one was the page's own instrumentation: the GX decode timer (P11) ran around every `fifo::process` call, ~8,700 `performance.now()` calls a frame; it now times one call in 64 and scales (`lib/web_perf.hpp`): 566 calls a frame. Leads, largest first: memory access (byte swaps have no WebAssembly instruction; the page-table load could become a mask for MEM1/MEM2 if the deferred-read and executable-write pages can be handled otherwise), aurora's per-draw state work (`handle_draw`, `resolve_pipeline_state`, hash lookups, `ByteBuffer::append`), indirect-call dispatch |
| P22 | To do (keep in mind): ubershaders | ~72 fixed-function states vs ~900+ shader configurations in the seed: an ubershader that reads TEV/texgen/lighting from per-draw data would cut pipelines to ~72, remove mid-game compiles and make a cold start seconds. Big; Safari must use it without a background compile of specialised shaders |
| P23 | To do (keep in mind): per-frame upload size | ~0.8-1.3 MB of vertex/uniform data per frame on the iPad (60 MB/s through Safari's GPU process); look at uniform padding and what is re-sent unchanged |
| P24 | To do (keep in mind): who can run it | WebGPU + JSPI means iPadOS/iOS 27 or Chrome on Android 12+ with Vulkan; older devices would need a WebGL renderer and no JSPI (Asyncify on a 128 MB module is not realistic) |
| S12 | Three ways to get the game's files | answer to "server, file drop, one big download": (1) served by a server, read over the network as it plays (`?data=`); (2) supplied by the player: disc image or folder, by button or by dropping it on the page (S10), optionally installed (S11); (3) download once: not one big .wasm (the files are 2.7 GB; a WebAssembly memory holding them would exceed what phones allow a tab, and the module would have to be recompiled), but `?install` from a server into the browser's storage (S11) followed by offline play. Missing for a complete (3): one archive instead of 2037 range downloads, and a service worker that keeps the page and the 28 MB module for offline starts. A public site must never serve the game's files (CONTRIBUTING: no Nintendo data); (1) and (3) are for your own server |
| P25 | Guest memory access on WebAssembly | investigated (2026-10-05), no change: it is already near the floor for this design. A 32-bit guest load is a page-table load (1 MiB pages, also routing MMIO, deferred EFB reads and executable-write guards to the slow path), an add and a 6-instruction byte swap; a store adds two range checks; the translator already resolves a host range once for runs of accesses off one base register (`ResolveRangeHost`, `ReadResolved*`/`WriteResolved*`) and keeps registers in locals. A direct MEM1/MEM2 mapping instead of the table would still need a range check to route MMIO, so it saves ~2 instructions and one cached load (the P6 conclusion). The one outlier is 64-bit swaps (doubles, FPR saves, paired-single loads): LLVM expands `bswap64` into ~30 instructions and folds any scalar rewrite back into it; a SIMD byte shuffle is 7 WebAssembly instructions but moves through vector registers, so it needs an on-device measurement, for ~1-2% at best. Real share of memory access is roughly 15% of CPU (the 28% of P21 includes out-of-line call overhead); the larger levers are aurora's per-draw work, indirect dispatch and, on Safari, shader compiles |
| P26 | Pipeline seed and compile concurrency per browser engine | done (2026-10-05) after the iPad showed what P20 cost there: shaders Safari had cached compiled in ~35 ms, new ones in ~200-300 ms each with the page stalled, so the 1150 extra seed entries added minutes to a cold Safari start (frames of 5-11 s per batch of 32). The web build now preloads both seeds and the loader picks one just before `main()`: WebKit-only browsers (Safari, every iOS browser) get the native seed and learn the rest per device; others get the web seed (Chrome: 2749 in ~14-18 s, compiled off the page). WebKit also keeps 4 asynchronous compiles in flight instead of 64 (`Module.mkwPipelinesInFlight`, read by `lib/gfx/pipeline_cache.cpp`), so the screen can update between stalls. Overrides: `?seed=native|web`, `?inflight=N`; `?resetpipelines` forgets a browser's learned pipelines (the iPad's cache had absorbed the 2749) |
