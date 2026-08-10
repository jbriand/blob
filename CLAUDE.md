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
| `net/` | **The wire format, not the transport.** `quantize.hpp` is the *only* place packing rules live, shared verbatim by both sides: position→u16 per axis against the world extent (~0.125 units/step at 8192), direction→i8 per axis, mass→u16. `protocol.hpp`: `protocol_version` (bump on any packing change; sides refuse to talk across a mismatch), `MessageId` (Hello/Input up, Welcome/Snapshot/Goodbye down), `Channel` split (Control = reliable ordered, Snapshot = unreliable), span-based `ByteWriter`/`ByteReader` — plain structs driven by free functions `write_u8/16/32`, `read_u8/16/32`, `written`, `remaining`, `exhausted` (no allocation, no exceptions — they set overflow/underflow flags instead of scribbling), and codecs `write_/read_input` (6 B) and `write_/read_welcome` (8 B); readers return `std::nullopt` on malformed input. M1 added the snapshot family — `EntityRecord` (13 B) + `SnapshotHeader`, `write_/read_entity_record`, `write_/read_snapshot`: self-contained chunks of ≤ `max_entities_per_chunk` = 91 against a 1200 B soft MTU, `protocol_version` = 2. net is standalone by decision: wire types are raw integers and this header includes no `<blob/sim/…>` — the numeric `EntityKind` mirror (`entity_kind_count`) is enforced by a static_assert in `test_snapshot.cpp`, the one place both modules link. M6 completed the control plane at `protocol_version` = 3: `HelloPayload` (version + ≤16-byte nickname in a fixed array — codecs never allocate) with `write_/read_hello`, and `GoodbyeReason` {VersionMismatch, ServerFull, Shutdown} with `write_/read_goodbye` (2 B) — version refusal is symmetric since v3. |
| `sim/` | The authoritative `World`, a plain struct of `entities` / `intents` / `next_id` / `tick` / `tuning` / `grid` driven by free functions `spawn(w, …)`, `apply_intent(w, …)` and `step(w, dt)`. `EntityId` u32 (monotonic, never reused, starts at 1 — 0 means "no entity"), `PlayerId` u16, `EntityKind` {Cell, Pellet, Virus, EjectedMass}, plain `Entity`, `PlayerIntent` (unit direction or `{0,0}`, split/eject flags — `apply_intent` upserts the latest intent per player). `step` is deterministic and frame-rate independent: intent → velocity via `speed_for_mass(tuning, m)`, integrate, clamp to the square, rebuild the grid. `tuning.hpp`: every gameplay constant as the `Tuning` aggregate (data, not scattered constexpr, so a server-side config file can override at startup while core stays I/O-free; `tick_dt(tuning)` and `radius_for_mass(tuning, m)` are derived functions, never stored companions). `spatial_grid.hpp`: uniform CSR `SpatialGrid`, counting-sort `rebuild` per step, `for_each_in_circle` (any radius — what eating must use) and `for_each_candidate_pair` (complete only for pair distance ≤ `grid_cell_size`); queries treat a never-rebuilt or hand-corrupted grid as empty. M3 landed the core loop: `Entity.dead` mark → `erase_if` compaction inside `step` (two grid rebuilds — post-integrate for eat queries, post-compact so the standing "grid = as of last step" contract holds), `StepEvents` as a `World` **field** (eats + deaths, cleared by the next step — field-not-return keeps step's signature stable), injected PRNG via `make_world(seed)` with raw-draw scaling (std distributions differ across stdlibs), pellet field self-restoring to `target_pellet_count`, decay `e^(−λ·dt)` floored at threshold, `spawn_player`/`despawn_player` (despawn also clears standing intent; disconnect ≠ death). Eat gates: pellets/ejected centre-within-radius (inclusive); cells need ratio ≥ `eat_ratio` **and** centre inside by `eat_depth_factor`·r_victim. M4's skill layer: `Entity.impulse` (decaying kick, exact-integral displacement `impulse·(1−e^(−λ·dt))/λ` — derivation in `world.cpp`) + `merge_cooldown`; one-shot action phase consumes intent split/eject flags then clears them; same-owner resolution is per-player all-pairs (≤16 cap makes O(k²) free, no grid-distance limit), push-apart only WHILE a cooldown runs, free overlap after so steered remerging works, elder id survives a merge; eject pays `eject_mass_cost`, the pellet carries `ejected_mass`, the difference evaporates. M5: virus field maintained like pellets (refill only adds); pop = the cell-vs-cell gates aimed at a virus → EatEvent + forced radial burst via the shared split tail, capped by `max_cells_per_player`; feeding — `virus_feed_count` EjectedMass hits launch a new virus along the feed direction, absorbed mass evaporates; `spawn_player` does bounded safe-spawn retries against threat cells. `interest.hpp` (M6): `view_radius` zoom curve + `collect_visible` grid query. |
| `tests/` | GoogleTest target `blob_core_tests`, roughly one `test_<module>.cpp` per header (`protocol.hpp`'s `Protocol.*` cases currently live in `test_quantize.cpp`), ctest label `core`, `PRE_TEST` discovery. `World.StepIsFrameRateIndependent` is the canary for invariant 3 below. |

### server/ — `blob-server` (target `blob_server`)

Headless authoritative host; links `blob::core` + `blob::enet` only and must run on a box
with no GPU/X11. `tick_loop.{hpp,cpp}`: `TickLoop` plain struct holding a fixed-timestep
accumulator, built by `make_tick_loop(tick_rate, max_catch_up)` (the factory is what clamps
both arguments — a zero rate would divide by zero) and driven by free functions
`pump(loop)` (whole ticks due) and `time_to_next_tick(loop)` (for sleeping); `max_catch_up`
clamp against the death-spiral, `ticks_run`/`ticks_dropped` counters. Because the state is
public, `pump()` treats a zero `step_duration` as "no ticks due" rather than dividing by it.
Tested by `blob_server_tests` (`server/tests/`, ctest label `server`), which compiles the
server sources directly — there is no library to link against.

The server is now a real host. `config.{hpp,cpp}`: pure `parse_config` (flat `key = value`,
`#` comments, CRLF-tolerant, unknown key = error) + thin `load_config_file`; validation pins
wire widths (`tick_rate` u8, `world_extent` u16); precedence CLI port > `blob-server.cfg` >
defaults; any error is line-numbered, printed in full, and fatal — a typo'd config never
runs on defaults. `session.{hpp,cpp}` (ENet-free, unit-testable): `PlayerSession` +
`sequence_newer` u16-wraparound compare; peers tagged via `peer->data` uintptr.
`snapshot_encode.{hpp,cpp}`: `collect_records` is the designated Entity→`EntityRecord` edge
(net stays standalone), `for_each_chunk` slices ≤91; `collect_visible_records` applies the
per-peer budget (nearest-first, deterministic dist²-then-index order). `main.cpp` loop:
drain socket → inject latched split/eject flags into stored intents (only when ticks are
due — earlier, a steering-only input could erase the press) → pump fixed steps (after each
step, `world.events.deaths` → `spawn_player` respawn for still-connected players — read per
step, the next step clears events) → per-peer interest-managed sends once per iteration
when ≥1 tick ran: visible set via `collect_visible` around the player's centroid at
`view_radius`, capped at `snapshot_chunks_per_tick` × 91 records, unreliable-sequenced
flags 0 → sleep inside `enet_host_service`, mid-sleep events routed through the same
`handle_event` as the drain. Incoming Input is dequantized then **re-normalized
unconditionally** (a hostile (127,127) would move √2 faster — the authoritative side
sanitizes intent); split/eject OR-latch into `pending_split`/`pending_eject`. The handshake
is half-open: connect = session only; a valid Hello (version match) stores the nickname,
spawns via `spawn_player` and sends Welcome; a mismatch earns a reliable
`Goodbye{VersionMismatch}` + disconnect — refusal is symmetric since v3. Disconnect =
`despawn_player`. The loopback tests drive a one-process client+server pair over real UDP
on 127.0.0.1:27877 through the entire connect→Hello→Welcome→Input→step→per-peer-snapshot→
decode chain, plus the refusal path.

### client/ — `blob-client` (target `blob_client`)

The **only** target allowed to see SFML. Connects over ENet (`blob-client [host] [port]
[nickname]`, default 127.0.0.1:7777 "player"), sends Hello (version + ≤16-byte nickname)
on connect, handles Goodbye reasons (printed human-readably before a clean exit), keeps
the Welcome version check belt-and-braces, then streams cursor → `normalized()` →
`quantize_direction` → `InputCommand` at the *Welcome's* tick rate (unreliable, channel 1,
`sequence++` per send). Space = split, W = eject: KeyPressed edges with key-repeat
disabled — each press rides exactly one InputCommand. Snapshots: latest-tick chunk assembly (newer replaces, equal
appends, older drops), positions dequantized against the extent **from Welcome**, radius
via `radius_for_mass(default_tuning, …)` (cosmetic divergence if a server config overrides
`radius_factor` — tuning sync is M6 territory), camera centred on the first own cell.
No interpolation yet — raw 20 Hz; the snapshot buffer + interpolation is the next client
iteration and belongs in core as pure logic. `WIN32_EXECUTABLE OFF` keeps the console
attached for netcode debugging.

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

**Feature-complete against the original roadmap** — M1 through M6 all landed 2026-08-10
across parallel branches (see each ROADMAP section's shipped annotation): movement, eating,
pellet + virus fields, split/eject/merge, safe spawn, per-peer interest-managed snapshots,
protocol v3 with a symmetric handshake, and a config file covering every knob. The
full-world-broadcast cliff is retired: ~12.9 kB/s per peer at spawn view, hard-capped at
71.4 kB/s by the chunk budget regardless of world size. Remaining tiers, all deliberate
deferrals: delta snapshots + id compaction (M7 — land when measurements demand), client
interpolation (pure logic, belongs in core), tuning sync to clients (needed once
client-side prediction exists), Hello timeout, spawn-protection ticks. The iteration plan
for the core lib is in [ROADMAP.md](ROADMAP.md) — keep both files updated as iterations
land.

**Plain-struct conversion is complete.** `Vec2`, `ByteWriter`/`ByteReader`, `TickLoop` and
`World` all follow the struct + free-function convention above; the only remaining classes
are the `EnetGuard` RAII wrappers in the two `main.cpp` files, which is deliberate. New
code is expected to match — see § Conventions.
