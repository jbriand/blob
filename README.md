# blob

agar.io clone — authoritative server, hand-rolled UDP protocol. C++23, clang, Ninja.

## Layout

```
core/     STATIC LIB — simulation + protocol. No SFML, no ENet, no I/O.
server/   headless authoritative host. core + ENet.
client/   SFML 3 renderer. core + ENet + SFML.
cmake/    compiler defaults, externals resolution, structural checks.
```

The `core` purity rule is enforced, not just documented: `blob_assert_no_transitive_deps()`
walks the target's transitive link closure (through ALIAS targets) at configure
time and hard-errors if ENet or SFML ever appear there.

## Build

```powershell
# Windows — from a Developer PowerShell so clang-cl finds the Windows SDK
cmake --workflow --preset windows-dev

# or step by step
cmake --preset windows-clang
cmake --build --preset windows-clang-debug
ctest --preset windows-clang-debug
```

```bash
# Linux
cmake --workflow --preset linux-dev

# what a game-server box / CI builds: no SFML in the graph at all
cmake --preset linux-clang-headless
cmake --build --preset linux-headless-debug
```

All presets use **Ninja Multi-Config**, so `Debug`, `RelWithDebInfo` and
`Release` share one configure and one build tree. `BLOB_WARNINGS_AS_ERRORS` is
ON in every preset; it defaults to OFF for anyone configuring by hand.

| Preset | Host | Targets |
|---|---|---|
| `windows-clang` | Windows | core, server, client, tests |
| `windows-clang-headless` | Windows | core, server, tests |
| `linux-clang` | Linux | core, server, client, tests |
| `linux-clang-headless` | Linux | core, server, tests |
| `linux-clang-asan` | Linux | as `linux-clang`, ASan + UBSan |

## Externals

FetchContent, in `cmake/blob_externals.cmake`. Three dependencies, pinned by
tag: ENet, SFML 3, GoogleTest.

**The download cache lives in `ext/`, not in the build trees.** FetchContent's
default (`${CMAKE_BINARY_DIR}/_deps`) would mean one clone per preset and a
re-download every time a build tree is wiped. `FETCHCONTENT_BASE_DIR` points at
`<source>/ext`, so all presets share one checkout that survives `rm -rf build/`.
It is gitignored.

Pins are cache variables — `BLOB_ENET_TAG`, `BLOB_SFML_TAG`,
`BLOB_GOOGLETEST_TAG` — so bumping one is a `-D` away. They are tags rather than
commit SHAs on purpose: `GIT_SHALLOW` cannot fetch a bare SHA against most
servers.

Escape hatches, so letting CMake own the checkout never means losing control of
it:

```powershell
# build against your own clone: no download, your edits are live
cmake --preset windows-clang -DFETCHCONTENT_SOURCE_DIR_ENET=C:/cpp/enet

# never touch the network; assume ext/ is already populated
cmake --preset windows-clang -DFETCHCONTENT_FULLY_DISCONNECTED=ON

# try find_package() first, download only if it fails (CI images, distro boxes)
cmake --preset linux-clang -DBLOB_USE_SYSTEM_PACKAGES=ON
```

Downstream targets link the normalized names — `blob::enet`, `blob::sfml`,
`GTest::gtest_main` — so an upstream target rename is a one-line fix in
`blob_externals.cmake`. ENet 1.3.x's CMake predates target-scoped include
directories, and that shim lives there too.

## Quantization

The snapshot stream is the bandwidth bill: every visible entity, to every
player, 20 times a second — each byte in a per-entity record gets multiplied by
entity count × 20 Hz × player count. So every field that crosses the wire is
quantized, and `core/include/blob/net/quantize.hpp` is the only place the
packing rules live; both sides share it verbatim.

**Lossy is safe because wire values are display-only.** The server keeps the
authoritative floats and resolves all gameplay (eat checks, speeds) against
them; snapshot values only ever drive rendering. The error budget is "can the
player see it", never "does gameplay break".

| Field | Encoding | Worst error |
|---|---|---|
| position | u16 per axis over the world square | ±0.06 units at extent 8192 |
| direction | i8 per axis | ±0.4% per axis |
| mass | u16, whole units, saturates at 65 535 | ±0.5 units |

Mass is **linear on purpose**, and the alternative it rejects is worth spelling
out. The drawn radius is `r = k·√mass`, so packing `√mass` into a byte gives
equal steps in *radius* — uniform resolution in the one quantity the player
actually perceives, at half the bytes:

| mass | linear u16 error | byte-ranged `√mass` error |
|---|---|---|
| 10 | ±0.5 | ±3 (~30%) |
| 100 | ±0.5 | ±10 (10%) |
| 4 000 | ±0.5 | ±63 (1.6%) |

The sqrt column's growing absolute error is the point — big cells may be coarse
because the eye cannot tell — but the small end is where it bites: representable
masses become roughly the perfect squares (1, 4, 9, 16, 25…), a HUD score
decoded from them jumps visibly, and pellet masses collapse onto a couple of
codes. One byte per entity is not worth that. If per-entity bytes ever become
the measured bottleneck, the bigger win is per-kind records — pellets dominate
entity counts and need neither `mass` nor `owner` fields at all — with
byte-ranged `√mass` as the fallback after that. Either change bumps
`protocol_version`.

## What's actually implemented

Enough to compile, link, run and test — not enough to play.

- `core/math` — `Vec2`, constexpr, zero-safe normalize.
- `core/net` — quantization helpers (position → 16 bits, direction → 8 bits),
  message ids, ENet channel split (reliable control / unreliable snapshot),
  `ByteWriter`/`ByteReader` that report overflow instead of scribbling, and the
  snapshot codec (protocol v2): 13 B entity records in self-contained chunks of
  at most 91 against a 1200 B soft MTU. net is standalone — wire types are raw
  integers; the `sim::EntityKind` mirror is asserted where the tests link both.
- `core/sim` — `World` with a frame-rate-independent `step(world, dt)`,
  mass-dependent speed, entity kinds; every gameplay constant lives in the
  `Tuning` aggregate (data, so a server config file can override it later);
  uniform CSR `SpatialGrid` rebuilt each step, with circle and candidate-pair
  queries. Collision and split/merge are `TODO`.
- `server` — fixed-timestep `TickLoop` with a catch-up clamp, ENet host that
  drains the socket before each tick and sleeps inside `enet_host_service` so a
  packet can wake it early.
- `client` — SFML 3 window, cursor → quantized intent, one placeholder circle.
- 51 GoogleTest cases across two targets — `blob_core_tests` (label `core`) and
  `blob_server_tests` (label `server`) — including a frame-rate-independence
  check on `step`, differential grid tests against brute force with an O(n²)
  tripwire, exhaustive snapshot-truncation rejection, and a catch-up-clamp
  check on the tick loop.

Data types here are plain structs and the operations on them are free functions
taking the struct first — `step(world, dt)`, `write_u8(w, v)`, `pump(loop)` —
rather than methods. Classes are reserved for RAII guards over C APIs, where a
destructor earns its keep.
