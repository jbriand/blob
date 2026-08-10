# ROADMAP — building out the `core` static lib

How `blob::core` gets from walking skeleton to playable. Each iteration is a shippable
slice: it ends with every preset green under warnings-as-errors, the purity check passing,
and new unit tests covering the new logic.

**Sequencing rationale.** M1 first because the snapshot codec is the only missing piece
between the skeleton and a networked playable build, and it is pure codec work with no
simulation risk. M2 before M3 because `world.cpp` forbids pairwise work without a broad
phase (invariant 6). M4 builds on M3's removal + events machinery. M5 and M6 can swap
depending on appetite: playtest fun (viruses) vs. scale (interest management).

Definition of done, every iteration:

- `cmake --workflow --preset windows-dev` green; Linux + ASan presets green when a Linux
  box is available.
- No new dependencies in `core` (the configure-time purity check stays the referee).
- Determinism suite extended, not just preserved (see cross-cutting rules at the bottom).
- README "What's actually implemented" + this file updated in the same change.

---

## M1 — Snapshot wire format  `core/net`  (size: S–M)

**Goal:** the server can encode world state, the client can decode it. Unblocks the first
end-to-end playable: cursor-chase over real UDP.

Ships:

- On-wire per-entity record — proposed layout, 13 B:
  `id u32 · owner u16 · kind u8 · pos_x u16 · pos_y u16 · mass u16`
  (owner included so the client can tell "mine" from "theirs" and color by player).
- Snapshot header: `MessageId u8 · tick u32 · count u16` (7 B). Tick truncates u64→u32 on
  the wire — ~6.8 years at 20 Hz, fine.
- `write_snapshot` / `read_snapshot` in the existing codec style: noexcept, overflow flag,
  `std::nullopt` on malformed.
- **Chunking from day one**: ~90 entities fit a soft 1200 B MTU budget, and a pellet field
  is far bigger than that. Each chunk is self-contained (repeats the tick, carries its own
  count) so a lost chunk on the unreliable channel degrades gracefully — entities are
  absolute state pre-delta, so partial application is benign.

Decisions:

- `quantize_mass` doc/impl mismatch — **settled ahead of M1**: linear u16 kept (exact
  small-mass display, cap 65 535 is plenty) and the header comment now matches the code.
  Rationale lives in README § Quantization; byte-ranged `√mass` stays the documented
  fallback and would be a `protocol_version` bump.

Tests: round-trip within quantization error bounds; truncated/garbage input → nullopt;
writer overflow sets the flag and writes nothing past the end; chunk-count arithmetic;
empty snapshot is legal.

Unblocks (outside core, not part of this iteration): the three server TODOs (Welcome on
connect, Input decode → `apply`, snapshot broadcast) and the client's snapshot buffer.

## M2 — Radius, tuning header, spatial grid  `core/sim`  (size: M)

**Goal:** the broad phase that invariant 6 demands, before any pairwise gameplay exists.

Ships:

- `sim/tuning.hpp`: one home for gameplay constants with why-comments — `base_speed`, the
  −0.44 speed exponent, radius factor, and every threshold M3/M4 will add. It also absorbs
  `tick_rate` / `tick_dt` / `world_extent`, which already sit at namespace scope in
  `world.hpp` — a definition move only, since call sites say `sim::tick_dt` today.
- `radius_for_mass(m) = k·√m` — needed by eating, by grid sizing, and eventually by client
  rendering.
- `SpatialGrid`: uniform buckets over the 8192² square, flat storage, rebuilt every step
  (everything moves every tick — rebuild beats incremental). Plain struct plus free
  functions, per the convention in CLAUDE.md — API sketch:
  `rebuild(grid, span<const Entity>)`, `for_each_in_circle(grid, center, r, fn)`,
  `for_each_candidate_pair(grid, fn)`.
- Wire the rebuild into `step()` at the `TODO(spatial)` mark (no behavior change yet).

Tests: differential vs. brute force O(n²) on seeded random layouts (uniform + clustered),
boundary/corner cells, empty world; a visited-candidate-pair count bound so an accidental
O(n²) regression fails loudly instead of just slowly.

## M3 — Eating, deaths, pellets, mass decay  `core/sim`  (size: M–L)

**Goal:** the core loop of the genre — eat to grow, die when eaten.

Ships:

- Entity removal: dead-flag during the step, end-of-step swap-remove compaction. Ids stay
  monotonic and are never reused; nothing may hold an index across a step.
- Eat rules: pellets/ejected mass always eatable; cell-vs-cell requires mass ratio ≥
  `eat_ratio` (~1.25) **and** the smaller's center inside the larger by an overlap factor.
  Mass transfers to the eater.
- `StepEvents` returned by `step(world, dt)` (eaten pairs, player deaths) so the server can
  respawn and send Goodbye without core knowing about sessions, and the client can do
  effects later.
- Pellet field: maintain `target_pellet_count` by respawning from an **injected PRNG** —
  a `make_world(seed)` factory with the generator as a field on `World`; determinism
  (invariant 3) is preserved because the seed is part of the input.
- Mass decay above a threshold, rate·dt.
- Player lifecycle API: `spawn_player` / `despawn_player` (naive placement now; safe-spawn
  polish waits for M5).

Tests: ratio/overlap boundary cases just-above and just-below; id stability across
compaction; pellet count restoration; decay is dt-independent; **replay determinism** —
same seed + same intent script → identical entity state after N ticks (same binary).

## M4 — Split, eject, merge  `core/sim`  (size: L)

**Goal:** the skill layer — multi-cell players.

Ships:

- One-shot action semantics: `split`/`eject` consumed edge-triggered, once per tick
  (the server latches a keypress into a single tick's intent).
- Split: per cell, requires `min_split_mass` and owner cell count < 16; halves mass,
  impulse along intent direction, starts a mass-scaled merge-cooldown timer on both halves.
- Eject: fixed mass chunk as `EjectedMass` with initial velocity and **dt-correct
  exponential damping** (`v·e^(−λ·dt)` — per-tick multiplicative factors would silently
  break frame-rate independence and the M1-era tests would catch it).
- Same-owner cells never eat each other: soft push-apart while cooldowns run, merge on
  contact once both timers expire.
- `Entity` grows timer state (a field, simplest — timers never cross the wire).

Tests: mass conservation through split; 16-cap enforced; merge gated by the cooldown;
eject travel distance independent of tick rate; the whole existing suite still green.

## M5 — Viruses & spawn polish  `core/sim`  (size: M, deferrable)

- Virus population maintenance; pop rule: a sufficiently bigger cell overlapping a virus
  bursts into pieces (respecting the 16 cap — this is the anti-snowball mechanic).
- Feeding: N ejected-mass hits make a virus split toward the feed direction.
- Safe spawn placement (grid query for a spot away from big cells), optional spawn
  protection ticks.

Explicitly optional before a first playtest — swap with M6 freely.

## M6 — Interest management & snapshot scale  `core/sim` + `core/net`  (size: M–L)

**Goal:** per-player snapshots that stay inside bandwidth as the world fills up.

Ships:

- Visible-set query: camera radius derived from total player mass (the zoom curve),
  answered by the grid.
- Per-peer snapshot budgeting: nearest-first fill within the MTU budget.
- Then delta snapshots against the last-acked tick — `InputCommand::sequence` is the
  ack groundwork; id compaction (u32 → u16 slots) only if measurements demand it.
- Protocol completion: Hello payload (nickname), Goodbye reason codes. Batch all M6 wire
  changes into **one** `protocol_version` bump.

Tests: visible set vs. brute force; budget never exceeded; delta round-trip including
recovery after dropped snapshots.

---

## Cross-cutting rules

- **Determinism is the load-bearing invariant.** No clocks, no global RNG, dt-scaled
  everything. The replay-determinism test from M3 gets extended with every new mechanic —
  it is the cheapest possible desync detector and, later, the foundation for client-side
  prediction.
- Float determinism is claimed **same-binary only**; cross-platform bit-exactness is a
  non-goal (never hash float state across platforms in CI).
- Tuning constants go to `sim/tuning.hpp` with a why-comment, never inline magic numbers.
- New types are plain structs with free functions taking the struct first (see CLAUDE.md
  § Conventions). Where an invariant used to lean on private state, re-establish it
  defensively — public fields mean no function may assume its inputs are sane.
- Wire-format changes cluster: batch them per iteration into one `protocol_version` bump.
