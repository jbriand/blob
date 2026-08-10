# blob — project instructions

agar.io clone — authoritative server, hand-rolled UDP protocol over ENet. C++23, clang,
Ninja Multi-Config. Three build products: a pure static lib (`core`), a headless server,
an SFML 3 client.

## Build & test

Windows (run from a **Developer PowerShell** so clang-cl finds the Windows SDK):

```powershell
cmake --workflow --preset windows-dev     # configure + build Debug + ctest, one shot
# or stepwise
cmake --preset windows-clang
cmake --build --preset windows-clang-debug
ctest --preset windows-clang-debug
```

Linux: same shape with `linux-dev` / `linux-clang` / `linux-clang-debug`.
Sanitizers: `linux-clang-asan` configure preset + `linux-asan-debug` build/test presets.

- Build trees: `build/<configure-preset>/`, binaries in `build/<preset>/bin/<Config>/`
  (e.g. `build/windows-clang/bin/Debug/blob-server.exe`). All disposable; `ext/` is not a
  build tree and survives wiping them.
- Every preset is Ninja Multi-Config (`Debug`/`RelWithDebInfo`/`Release` share one configure)
  and sets `BLOB_WARNINGS_AS_ERRORS=ON`. Keep new code warning-clean under **both** frontends:
  clang-cl (`/W4 /permissive-`) and clang++ (`-Wall -Wextra -Wpedantic -Wshadow -Wconversion
  -Wsign-conversion -Wold-style-cast -Wdouble-promotion …`).
- Component toggles: `BLOB_BUILD_SERVER` / `BLOB_BUILD_CLIENT` / `BLOB_BUILD_TESTS` (default ON).
  The `*-headless` presets are `BLOB_BUILD_CLIENT=OFF` — what CI and a game-server box build.

## Repository map

```
blob/
├── CMakeLists.txt        root: options, C++23 globals, includes cmake/ modules,
│                         runs the core-purity assertion (see Invariants)
├── CMakePresets.json     configure/build/test/workflow presets described above
├── cmake/                build-system modules, one concern per file
├── core/                 STATIC LIB blob::core — simulation + protocol, pure
├── server/               exe blob-server — headless authoritative host
├── client/               exe blob-client — SFML 3 renderer
├── ext/                  FetchContent download cache (created on first configure,
│                         gitignored, never edit by hand)
└── build/, install/      per-preset output trees, disposable
```

### cmake/ — build-system modules

| File | Provides | Notes |
|---|---|---|
| `blob_compiler.cmake` | `blob_set_target_defaults(t)` | The one place for warning flags, driver-aware (clang-cl vs clang++), C++23/no-extensions, PIC, `NOMINMAX`/`WIN32_LEAN_AND_MEAN` on Windows. **Every new target must go through it.** |
| `blob_checks.cmake` | `blob_assert_no_transitive_deps(t FORBIDDEN …)` | Walks the target's transitive link closure (through ALIAS targets) at configure time; hard-errors if a forbidden target appears. |
| `blob_externals.cmake` | `blob_bring_in_externals()` | FetchContent for ENet, SFML 3, GoogleTest. Pins are cache vars (`BLOB_ENET_TAG` etc., tags not SHAs — `GIT_SHALLOW` can't fetch bare SHAs). Cache in `<source>/ext` shared by all presets. Exposes normalized names `blob::enet`, `blob::sfml`, `GTest::gtest_main` so upstream renames are a one-line fix. Two upstream shims live here: the ENet include-dir patch (its CMake predates target-scoped includes), and `-Wno-character-conversion` scoped to gtest's own targets (v1.15.2 builds itself with `-WX`, and its `char8_t` printer overload — which only exists from C++20 on — trips a newer clang; raising the standard cannot help, since C++23 is what enables the overload). Escape hatches: `FETCHCONTENT_SOURCE_DIR_<DEP>` (use your own clone), `FETCHCONTENT_FULLY_DISCONNECTED=ON` (never touch network), `BLOB_USE_SYSTEM_PACKAGES=ON` (try `find_package` first). |

### core/ — `blob::core` static lib (pure simulation + protocol)

**Hard rule: no ENet, no SFML, no sockets, no filesystem, no clocks, no stdout.** Enforced
at configure time by the purity assertion in the root CMakeLists — a forbidden dependency
fails the configure, not the deploy. Everything here must build and unit-test in isolation.
This is where nearly all gameplay work lands.

Public headers live in `core/include/blob/<module>/`, included as `<blob/module/x.hpp>`;
`core/src/` mirrors the same layout.

| Module | Purpose |
|---|---|
| `math/` | `Vec2`: constexpr plain struct (`is_aggregate`/`is_trivially_copyable` asserted in `vec2.cpp`), with every operation beside it as a free function — non-member arithmetic operators, `dot`/`length_sq`/`length`, zero-safe `normalized()` (returns `{0,0}` instead of NaN — a cursor exactly on the cell is normal input, not an error). |
| `net/` | **The wire format, not the transport.** `quantize.hpp` is the *only* place packing rules live, shared verbatim by both sides: position→u16 per axis against the world extent (~0.125 units/step at 8192), direction→i8 per axis, mass→u16. `protocol.hpp`: `protocol_version` (bump on any packing change; sides refuse to talk across a mismatch), `MessageId` (Hello/Input up, Welcome/Snapshot/Goodbye down), `Channel` split (Control = reliable ordered, Snapshot = unreliable), span-based `ByteWriter`/`ByteReader` — plain structs driven by free functions `write_u8/16/32`, `read_u8/16/32`, `written`, `remaining`, `exhausted` (no allocation, no exceptions — they set overflow/underflow flags instead of scribbling), and codecs `write_/read_input` (6 B) and `write_/read_welcome` (8 B); readers return `std::nullopt` on malformed input. |
| `sim/` | The authoritative `World`, a plain struct of `entities` / `intents` / `next_id` / `tick` driven by free functions `spawn(w, …)`, `apply_intent(w, …)` and `step(w, dt)`. `EntityId` u32 (monotonic, never reused, starts at 1 — 0 means "no entity"), `PlayerId` u16, `EntityKind` {Cell, Pellet, Virus, EjectedMass}, plain `Entity`, `PlayerIntent` (unit direction or `{0,0}`, split/eject flags — `apply_intent` upserts the latest intent per player). `step` is deterministic and frame-rate independent: intent → velocity via `speed_for_mass` (placeholder `m^-0.44` curve), integrate, clamp to the 8192² square. `TODO(spatial)` in `world.cpp` marks where grid + collision must go. Constants at namespace scope, not on the struct: `tick_rate` 20 Hz, `tick_dt`, `world_extent`. |
| `tests/` | GoogleTest target `blob_core_tests`, roughly one `test_<module>.cpp` per header (`protocol.hpp`'s `Protocol.*` cases currently live in `test_quantize.cpp`), ctest label `core`, `PRE_TEST` discovery. `World.StepIsFrameRateIndependent` is the canary for invariant 3 below. |

### server/ — `blob-server` (target `blob_server`)

Headless authoritative host; links `blob::core` + `blob::enet` only and must run on a box
with no GPU/X11. `tick_loop.{hpp,cpp}`: `TickLoop` plain struct holding a fixed-timestep
accumulator, built by `make_tick_loop(tick_rate, max_catch_up)` (the factory is what clamps
both arguments — a zero rate would divide by zero) and driven by free functions
`pump(loop)` (whole ticks due) and `time_to_next_tick(loop)` (for sleeping); `max_catch_up`
clamp against the death-spiral, `ticks_run`/`ticks_dropped` counters. Because the state is
public, `pump()` treats a zero `step_duration` as "no ticks due" rather than dividing by it.
Tested by `blob_server_tests` (`server/tests/`, ctest label `server`), which compiles
`tick_loop.cpp` directly — there is no library to link against. `main.cpp`: RAII `EnetGuard`,
host on udp/7777 (argv[1] overrides), 64 peers, 2 channels; loop = drain socket → pump
fixed steps → sleep *inside* `enet_host_service` so an arriving packet wakes it early.
Open TODOs here: assign PlayerId + send Welcome on connect, decode Input → `apply_intent()`,
snapshot broadcast (blocked on the snapshot codec — see ROADMAP M1).

### client/ — `blob-client` (target `blob_client`)

The **only** target allowed to see SFML. SFML 3 window (1280×720, vsync, Esc quits);
per frame: cursor → centre delta → `normalized()` → `quantize_direction` → `InputCommand`
(built but not yet sent). Rendering is deliberately boring — one placeholder circle until
snapshots drive the view; the interesting client work is the future snapshot buffer +
interpolation. `WIN32_EXECUTABLE OFF` keeps the console attached for netcode debugging.

## Invariants (do not break)

1. **`core` purity** — no I/O of any kind in `core`. Machine-enforced at configure time.
2. **The server is authoritative; the client sends intent, never position.**
3. **`sim::step(world, dt)` is deterministic and frame-rate independent.** No clock reads, no
   global RNG (randomness must arrive as an injected seeded PRNG), everything dt-scaled —
   including future damping/decay terms (use `e^(−λ·dt)` forms, never per-tick factors).
4. **`quantize.hpp` is the sole packing authority**, and any wire-format change bumps
   `protocol_version`.
5. **Channel discipline**: session state on reliable Control, the 20 Hz stream on
   unreliable Snapshot — a dropped snapshot is always better than a late one.
6. **No broad-phase-free pairwise work in `step()`** — thousands of pellets make O(n²)
   fatal; the uniform grid must exist before collision/eat lands.
7. **Codecs never throw and never scribble**: writers set an overflow flag, readers return
   `std::nullopt` / set underflow. The cursor state is public, so this is a convention the
   tests police rather than something the type system forbids — which means nothing may
   assume a sane `offset`. Bounds checks are spelled `offset >= size`, never
   `offset + 1 > size` (that wraps to `0 > size` at `SIZE_MAX` and sails through into an
   out-of-bounds access), and `written`/`remaining` saturate. A corrupted offset must yield
   a wrong-but-defined answer and a flag, never a stray read or write.

## Quantization notes

Wire values are **display-only**: the server keeps the authoritative floats and resolves
all gameplay against them, so quantization error is a rendering concern, never a
correctness one. Current encodings: position u16 per axis (±0.06 units at extent 8192),
direction i8 per axis, mass u16 in whole units — **deliberately linear**, keeping small
masses (HUD score, pellets) exact where a byte-ranged `√mass` would step visibly
(representable masses ≈ the perfect squares). If per-entity bytes ever become the measured
bottleneck: per-kind records first (pellets need neither `mass` nor `owner`), byte-ranged
`√mass` second (uniform radius resolution, since drawn radius is `r = k·√m`). Full
rationale in the README's "Quantization" section; any such change bumps `protocol_version`.

## Conventions

- **Plain structs, free functions.** Types are `struct`s with public fields and NSDMIs — no
  private state, no member functions. What would have been a method is a free function in
  the same namespace taking the struct as its first parameter (`pump(loop)`,
  `write_u8(w, v)`), found by ADL. Operator overloads stay, as non-members. A constructor
  that did real work becomes a `make_*` factory (`make_tick_loop`). Constants that would
  have been `static` members go to namespace scope. Two standing exceptions: RAII guards
  over C APIs (`EnetGuard`) stay classes, because a destructor buys safety a free function
  cannot; and wherever private state used to enforce an invariant, the invariant has to be
  re-established defensively instead — see invariant 7 for the worked example.
- Namespaces mirror folders: `blob::math` / `blob::net` / `blob::sim`; server-local code
  in `blob::server`. Public includes angle-style `<blob/…>`, target-local ones quoted.
- `constexpr`, `noexcept`, `[[nodiscard]]` wherever they hold; designated initializers for
  aggregates; no exceptions on hot paths.
- Executable targets are `blob_<name>` with `OUTPUT_NAME blob-<name>`.
- New third-party deps only via `blob_externals.cmake`, pinned, behind a `blob::` alias.
- Core code and its tests land in the same iteration; comments explain *why* (invariants,
  tuning intent), not *what*.

## Status & roadmap

Walking skeleton: everything compiles, links, runs and tests green — not yet playable.
Implemented: movement sim, quantization, Input/Welcome codecs, tick loop, window + intent
capture. Missing: snapshot codec, spatial grid, collision/eating, split/merge, viruses,
interest management. The iteration plan for the core lib is in [ROADMAP.md](ROADMAP.md) —
keep both files updated as iterations land.

**Plain-struct conversion is complete.** `Vec2`, `ByteWriter`/`ByteReader`, `TickLoop` and
`World` all follow the struct + free-function convention above; the only remaining classes
are the `EnetGuard` RAII wrappers in the two `main.cpp` files, which is deliberate. New
code is expected to match — see § Conventions.
