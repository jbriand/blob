# Simulation math

The formulas behind `sim::step()` — speed, radius, eating, decay, the fixed timestep and
the broad-phase grid — each derived rather than asserted, with the tests that pin them.
Constants come from the `Tuning` aggregate
([data-structures.md](data-structures.md#tuning)); source is
[`core/src/sim/world.cpp`](../core/src/sim/world.cpp) unless noted.

## Speed curve

```
v(m) = base_speed · (max(m, 10) / 10) ^ speed_mass_exponent
     =       720  · (max(m, 10) / 10) ^ (−0.44)                    (defaults)
```

| mass | speed (units/s) |
|---|---|
| ≤ 10 | 720.0 |
| 100 | ≈ 261.4 |
| 1000 | ≈ 94.9 |

**The exponent must stay negative** — big has to be slow. With a positive (or zero)
exponent the biggest cell would also be the fastest, nothing could ever escape it, and the
genre collapses into "first to snowball wins on rails"; the mass→speed falloff is what
makes size a trade-off instead of a pure win. (Pinned by `World.HeavierCellsAreSlower` and
the sign check in `Tuning.DefaultsAreSane`.)

**The anchor-at-mass-10 clamp**: `base_speed` is *defined* as the speed of a mass-10 cell,
and `max(m, 10)` pins the curve to that anchor — nothing lighter than 10 moves faster than
`base_speed`, so shedding mass below the spawn weight can never become a speed exploit.
Note the anchor is the literal `10.0f` in `speed_for_mass`, not `tuning.spawn_mass`; they
merely coincide at the defaults.

## Radius

```
r(m) = radius_factor · √m  =  4·√m                                 (default)
```

√mass keeps the drawn **area proportional to mass**: area = π·r² = π·k²·m. Eating a
mass-100 meal then reads as the same visual growth whether you weigh 200 or 2000 — and
mass, the gameplay quantity, stays honestly represented by the pixels players actually
judge distances with. Examples: r(10) ≈ 12.65, r(100) = 40, r(400) = 80. Zero-safe:
negative mass (representable, since fields are public) yields r = 0, not NaN.

## Eat geometry

Resolution runs inside `step()`, over the grid built on this tick's post-integration
positions. Two rules, both evaluated from the **eater's** perspective:

- **Pellets / ejected mass**: eaten iff `dist ≤ r_eater` — the centre touches the rim,
  boundary **inclusive**. The grid query's `length_sq ≤ r²` filter *is* the rule, so the
  eat convention and the grid convention cannot drift apart.
- **Cells** (different owner only): eaten iff **both**
  1. `m_eater ≥ eat_ratio · m_victim` (ratio gate, 1.25), and
  2. `dist ≤ r_eater − eat_depth_factor · r_victim` (depth gate, factor ⅓).

The depth gate, rearranged: `r_eater − dist ≥ ⅓·r_victim` — the victim's **centre must sit
inside the eater's rim by at least a third of the victim's own radius**:

```
        eater (r_e)                        victim centre penetration:
      .-----------.                        r_e − dist  ≥  ⅓ · r_v
    /               \
   |        C        |   d = dist(C, V)    rim contact (dist = r_e) is
   |          \      |                     deliberately NOT enough: you
    \          V....pass line              commit to the overlap, which
      `-----------'\                       keeps near-misses readable
                    victim (r_v)           and escapes possible
```

Worked numbers (from [`core/tests/test_eating.cpp`](../core/tests/test_eating.cpp)):

- ratio gate, victim mass 80: threshold = 1.25·80 = **100** — a 99.9 eater starves, a
  100.1 eater feasts.
- depth gate, 200 vs 100 at ratio 2: limit = 4·√200 − ⅓·(4·√100) = 56.5685 − 13.3333 ≈
  **43.2352** — a victim centre at distance 43.29 survives, at 43.18 it does not.
- pellet rim, eater mass 100: reach exactly **40** — a pellet at distance 40 is a meal, at
  40.5 it is not.

**Mutual eating is impossible** because the ratio gate cannot hold both ways: it would
need `m_a ≥ 1.25·m_b` *and* `m_b ≥ 1.25·m_a`, hence `m_a ≥ 1.5625·m_a` — false for any
positive mass. Strictly-greater-than-1 is the point of `eat_ratio`: at 1.0, near-equal
cells would eat each other on touch and every skirmish would end in a coin flip; "bigger"
has to be a real, earnable edge before it grants a kill.

**Determinism of resolution**: eaters run in entity-array order, and that order *is* the
tie-break rule. Array order = spawn order (compaction is stable), spawn order is part of
the replayed input, so contested meals resolve identically on every replay
(`Eating.ChainResolvesInArrayOrderAndConservesMass` — mass transfers to the eater, the
total never leaks).

**Pre-growth radius, live-mass ratio**: an eater's reach `r_e` is computed once at its
scan start, so mass gained mid-scan does not extend its arm until the next tick — one meal
cannot cascade into a longer reach within a single tick. The *ratio* gate, by contrast,
reads live mass, so a cell that just grew does count as heavier when it is somebody's
potential victim later in the same scan. Either convention would be deterministic; this
pair just keeps intra-tick behavior tame.

Victims are only *marked* (`dead = true`) during the scan — removal would misalign entity
indices with the grid — and compacted away before `step()` returns. Deaths are then
derived by set-difference: owners holding a live Cell before eat resolution, minus owners
still holding one after; sorted-unique, so a player losing every cell in one tick dies
exactly once, and disconnects (which remove cells without a step) never read as kills.

## Mass decay

Each step, for **Cells above the threshold only**:

```
if m > decay_threshold:   m ← max(decay_threshold, m · e^(−λ·dt))       λ = decay_rate = 0.002/s
```

Cells at or below the threshold are untouched (the gate matters: the `max` formula alone
would *raise* them to the threshold). Decay is an anti-snowball drag, not a diet — it
taxes only mass above the line, floors exactly at it (an assignment, so the tests compare
`==`), and therefore can never starve anyone: dying takes being eaten.

**Why `e^(−λ·dt)` and not a per-tick factor — the dt-independence proof.** Slice one
second into n steps of h = 1/n each. After n multiplicative steps:

```
m · (e^(−λh))^n  =  m · e^(−λ·h·n)  =  m · e^(−λ·1)        — independent of h.
```

More generally `(e^(−λh))^(t/h) = e^(−λt)` for any step size h: exponentials compose
across any dt split, which is exactly invariant 3's demand. The broken alternative — a
constant per-tick factor `f` — gives `m · f^(t/h)`, which *depends on h*: at 20 Hz with
f = 0.999 a second costs `0.999²⁰ ≈ 0.9802`, at 40 Hz `0.999⁴⁰ ≈ 0.9608` — double the
tick rate, double the tax. Any future damping term (M4's ejected-mass velocity) must use
the same `e^(−λ·dt)` form.

`Decay.IsFrameRateIndependent` checks 10×0.1 s against 100×0.01 s with `EXPECT_NEAR`
(tolerance 0.01), **not** exact equality: the two paths round differently in float, and
bit-equality across different dt splits is not something `step()` promises — determinism
(invariant 3) is same-call-sequence, frame-rate independence is same-result-within-float-noise.
The absolute value is pinned too: 1000·e^(−0.002) ≈ 998.002.

## Fixed timestep

[`server/src/tick_loop.cpp`](../server/src/tick_loop.cpp). Real elapsed time goes into an
accumulator and comes back out in whole simulation steps:

```
accumulator += now − last;  last = now
steps        = min( floor(accumulator / step_duration), max_catch_up )
accumulator -= steps · step_duration
if accumulator ≥ step_duration:                      # still behind after the budget
    ticks_dropped += floor(accumulator / step_duration)
    accumulator    %= step_duration                  # backlog discarded, not banked
```

`step_duration = 1 s / tick_rate` (50 ms at 20 Hz), `max_catch_up = 5`. The clamp is the
death-spiral guard: after a stall, simulating the whole backlog takes longer than real
time, which grows the backlog further — so beyond 5 ticks (250 ms) of debt, the rest is
dropped and counted. A 2 s stall thus pumps 5 ticks and drops ~35
(`TickLoop.CatchUpClampDropsTheBacklogInsteadOfSpiralling`).

**Why integration is frame-rate independent**: movement is `position += velocity · dt`
with velocity constant within a step, so over a span T sliced any way,
`Σ v·dt_i = v·Σ dt_i = v·T` — the slicing cancels. The canary is
`World.StepIsFrameRateIndependent`: 10 steps of 0.1 s vs 100 steps of 0.01 s must land
within 0.5 units after ~720 units of travel (float rounding differs across slicings; see
the decay note above). The test runs with an empty pellet field on purpose: eating a
pellet quantizes a mass (and hence speed) change to a tick boundary, which is inherent to
discrete meals, not a dt bug — the canary asserts that the *continuous* dynamics are
dt-scaled.

## The spatial grid

[`core/include/blob/sim/spatial_grid.hpp`](../core/include/blob/sim/spatial_grid.hpp);
layout and worked CSR example in
[data-structures.md](data-structures.md#spatialgrid--gridentry). Invariant 6 forbids
pairwise work without a broad phase: with ~2000 pellets, all-pairs is n²/2 ≈ 2.1 M checks
per tick, and it grows quadratically.

**Why `cell_size` bounds candidate-pair completeness.** For any two points at distance
d ≤ cell_size: per axis, `|x₁ − x₂| ≤ d ≤ cell_size`, and points at most one cell-width
apart on an axis land in bucket columns at most 1 apart
(`⌊x₁/s⌋` and `⌊x₂/s⌋` with `|x₁−x₂| ≤ s` cannot differ by 2). So both points sit within
one bucket of each other on each axis — inside the 3×3 neighbourhood — and
`for_each_candidate_pair`, which scans each bucket against itself plus the half
neighbourhood (+1,0), (0,+1), (−1,+1), (+1,+1), reports every such pair exactly once
(each adjacent bucket pair is scanned from exactly one side). Beyond `cell_size` the
guarantee ends, which is why anything with a longer reach — a big cell's eat radius —
must use `for_each_in_circle`, which walks the whole covered cell rectangle instead.
Eat resolution does exactly that (invariant 6's "documented big-eater path").

**O(n²) → O(n·density), with numbers.** Uniform n entities over c×c buckets give density
ρ = n/c² per bucket; each entity is compared against its own bucket and 8 neighbours, each
unordered pair once, so candidates ≈ 9ρn/2. Defaults (extent 8192, cell 256 → c = 32) with
the tripwire test's n = 2048: ρ = 2 → expected ≈ 9·2·2048/2 = **18 432** pairs (the test
suite measured ~17.8 k). The tripwire bound is **40n = 81 920**: comfortably above
seed-clumping noise (~4.4× the uniform expectation) yet 25× below the all-pairs
n(n−1)/2 = 2 096 128 an accidental broad-phase regression would produce — it fails loudly,
not just slowly (`SpatialGrid.CandidatePairCountStaysNearLinear`).

**Counting-sort stability = determinism.** `rebuild` places entries by walking the input
span in order, so every bucket lists its entries in input order and the whole grid is a
pure function of the input span — same span, bit-identical `starts` and `entries`
(`SpatialGrid.RebuildIsDeterministicAndReusesStorage`). Since eat resolution iterates
eaters in array order but discovers *victims* through grid queries, this stability is what
keeps discovery order — and therefore contested-meal outcomes — replayable.

The grid is rebuilt twice per step: after integration (what eat queries see) and after
compaction (so the standing contract — "the grid describes the world as of the last
`step()`" — holds between steps; despawns make it stale until the next step, by contract).
Two O(n) rebuilds are the obviously-correct choice over cleverness about staleness — an
optimization candidate, not debt.

## Determinism & replay

Invariant 3: `step(world, dt)` is deterministic and frame-rate independent. What that
demands, concretely:

- **No clock reads** anywhere in `core` — time only enters as the explicit `dt`.
- **No global RNG** — randomness arrives as the seeded `std::mt19937` injected via
  `make_world(seed)`; the seed is part of the replayed input.
- **Everything dt-scaled** — integration linearly, decay exponentially (`e^(−λ·dt)`).
- **Lifecycle calls are part of the input sequence**: `spawn_player` and pellet respawn
  draw from `world.rng`, so a replay must repeat every `spawn_player` / `despawn_player` /
  `apply_intent` / `step` in the same order — the world is a pure function of
  (seed, call sequence).

**Same-binary only**: exact float equality is claimed for the same binary executing the
same instruction stream. Cross-platform bit-exactness is explicitly a non-goal (different
libm, different FP contraction) — never hash float state across platforms.

**Why std distributions are banned in `core`**: `std::uniform_real_distribution` and
friends have implementation-defined output — the same seed replays *differently across
standard libraries*. So `world.cpp` draws raw: `rand_unit` takes the generator's top 24
bits, `(g() >> 8) · 2⁻²⁴`, giving a uniform float in [0, 1) in which every value is
exactly representable (float carries a 24-bit significand) — deterministic today,
portable-by-construction if the same-binary restriction is ever relaxed. (Tests may use
distributions — the rule binds core code; test expectations are differential or
structural, never pinned to a distribution's output.)

**The enforcement**: `Replay.SameSeedAndScriptReplayIdentically` runs a 200-tick script —
two players steering on a schedule, a planted kill, a live 2000-pellet field, and mid-run
lifecycle churn (despawn at tick 100, respawn at 120) — twice from `make_world(424242)`,
then asserts **exact equality** (`EXPECT_EQ`, no tolerance) of tick, `next_id`, the full
PRNG state (`a.rng == b.rng` — the generators advanced in lockstep), and every entity's
id, owner, kind, position floats and mass. It is the cheapest desync detector the project
has and, later, the foundation for client-side prediction; every new mechanic is expected
to extend it.

## Quantization error bounds

Wire values are display-only (the server keeps the authoritative floats), so these bound
*rendering* error, never gameplay ([protocol.md](protocol.md#quantization) has the full
encoding story):

| Quantity | Step | Worst error (half a step) | Scale check |
|---|---|---|---|
| position (extent 8192) | 8192/65535 ≈ 0.125 units | **0.0625 units** | the smallest cell (mass 10) has radius ≈ 12.65 — error is ~0.5 % of it |
| direction | 1/127 per axis | ≈ 0.39 % per axis | server re-normalizes anyway |
| mass | 1 unit | **0.5 units** | every current mass is a whole number — exact on the wire |

Pinned end-to-end (through the real codecs) by `Snapshot.QuantizedFieldsStayWithinErrorBounds`,
which uses exactly `half_step = 0.5·extent/65535` and 0.5 as tolerances.
