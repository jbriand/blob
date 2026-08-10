# Simulation math

The formulas behind `sim::step()` — speed, radius, eating, decay, the impulse glide,
split/merge, the eject ledger, viruses, safe spawn, the zoom curve, the fixed timestep and
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
positions. Three rules, all evaluated from the **eater's** perspective:

- **Pellets / ejected mass**: eaten iff `dist ≤ r_eater` — the centre touches the rim,
  boundary **inclusive**. The grid query's `length_sq ≤ r²` filter *is* the rule, so the
  eat convention and the grid convention cannot drift apart.
- **Cells** (different owner only — same-owner pairs are invisible to the eat pass; merge
  and push-apart, [below](#split--merge), own them): eaten iff **both**
  1. `m_eater ≥ eat_ratio · m_victim` (ratio gate, 1.25), and
  2. `dist ≤ r_eater − eat_depth_factor · r_victim` (depth gate, factor ⅓).
- **Viruses**: the same two cell gates aimed at a virus (viruses are unowned, so the owner
  clause drops out) — the pop rule, worked through in [Viruses](#viruses). Passing both
  eats the virus *and* bursts the eater.

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
(Eating is still the only killer: a merge fuses two cells of the *same* owner — the owner
keeps the elder — and a pop leaves the popped player more cells, not fewer.)

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
tick rate, double the tax. Every damping term added since uses the same form — M4's
impulse and ejected-mass flight (`impulse_damping_rate`) — but a damped term that also
*moves* needs one further step: the exact displacement integral
([next section](#the-impulse-glide)).

`Decay.IsFrameRateIndependent` checks 10×0.1 s against 100×0.01 s with `EXPECT_NEAR`
(tolerance 0.01), **not** exact equality: the two paths round differently in float, and
bit-equality across different dt splits is not something `step()` promises — determinism
(invariant 3) is same-call-sequence, frame-rate independence is same-result-within-float-noise.
The absolute value is pinned too: 1000·e^(−0.002) ≈ 998.002.

## The impulse glide

M4's split kick and every ejected pellet's flight are decaying velocities,
`v(t) = v₀·e^(−λt)` with λ = `impulse_damping_rate` = 3.5/s. Damping the *speed* per step
is exactly the decay proof above; the new subtlety is the **displacement**, because the
naive step

```
position += v·dt;   v ← v·e^(−λ·dt)            # rectangle rule — breaks invariant 3
```

makes travel depend on the slicing. Over T = 1 s at λ = 3.5, the rectangle sums to
`v·dt·(1−e^(−λT))/(1−e^(−λ·dt))`: dt = 0.1 travels **0.3284·v₀**, dt = 0.01 travels
**0.2820·v₀** — ~16 % more distance at the coarser slicing from the *same* mechanic. (Each
rectangle overestimates by however much the velocity decayed inside the step, and coarser
steps decay more inside.) So `step()` integrates the decay **exactly**:

```
per-step displacement ("glide")   ∫₀^dt v·e^(−λt) dt  =  v·(1 − e^(−λ·dt))/λ
then the decay                    v ← v·e^(−λ·dt)
```

This composes across any slicing — the telescoping mirror of the decay proof:

```
Σₖ v·e^(−λ·k·dt) · (1 − e^(−λ·dt))/λ  =  v·(1 − e^(−λT))/λ        — independent of dt.
```

Total glide from a kick is therefore v₀/λ as T → ∞: a split half glides
≈ 780/3.5 ≈ **223 units** past its steering, an ejected pellet ≈ 1400/3.5 = **400 units**.
`Eject.TravelIsFrameRateIndependent` pins both halves of the claim: 10×0.1 s vs 100×0.01 s
land within 0.5 units of each other, *and* the absolute distance matches the closed form
1400·(1−e^(−3.5))/3.5 ≈ 387.9 (one second in, ≈ 97 % of the full glide already flown).

λ → 0 needs care — the formula reads (1−e⁰)/0 — and the code takes the limit explicitly:
`lim (1−e^(−λ·dt))/λ = dt`, which is also exactly right, because with no damping the
rectangle rule *is* the exact integral.

Where it applies: `Entity.impulse` — a **separate field from `velocity`**, because intent
overwrites `velocity` every tick and a kick stored there would vanish after one step; the
glide is *added* during integration — and, for `EjectedMass` only, `velocity` itself:
ejecta have no intent, so their whole flight lives there and decays by the same λ.

## Split & merge

**Split** (per cell, on a latched one-shot flag): requires `mass ≥ min_split_mass` (36)
and owner cell count < `max_cells_per_player` (16). The scan iterates only the cells that
existed before any splitting this tick, so a fresh half can never re-split off the same
keypress; and when the cap truncates a multi-cell split, array (= spawn) order is the
deterministic priority (`Split.CapTruncationFollowsArrayOrder`). The flags are edges:
`step()`'s action phase performs, then clears, so one latched press yields exactly one
action however many ticks follow — steering, by contrast, is a level and persists.

**Halving is exact.** `half = mass · 0.5f` only decrements the float exponent, so
half + half reassembles the parent's mass **to the bit** — conservation by construction,
compared with `==`, no tolerance (`Split.HalvesConserveMassExactlyAndBothCarryTheCooldown`).
The new half launches with `impulse = intent · split_impulse_speed` (780 u/s); a zero
intent still splits — in place, with push-apart providing the separation along the +x
tiebreak.

**Merge cooldown, mass-scaled:**

```
cooldown(m_piece) = merge_cooldown_base + merge_cooldown_per_mass · m_piece
                  =        10 s         +      0.02 s/unit        · m_piece
```

Both halves are armed with it (50-mass halves: **11 s**) — big splits stay committed
longer, the anti-"split, eat, instantly reassemble" rule. M4's player split and M5's pop
burst share the single `merge_cooldown_for` formula on purpose, so the mass-scaled
commitment cannot drift between them. Timers are dt-decremented and floored **by
assignment at exactly 0.0f**, which is why the merge gate's `== 0.0f` is a precise
"expired" test, not a float hazard.

**The merge window**: a same-owner pair fuses when both cooldowns have expired **and**

```
dist ≤ merge_overlap · max(r_a, r_b)             (0.25 · the bigger radius)
```

Worked (`Merge.NeedsDeepOverlapNotMereTouch`): masses 100 (r 40) and 25 (r 20) merge only
within 0.25·40 = **10 units** — centre distance 10.5 stays two cells, 9.5 fuses. The elder
(lower id) survives in place and absorbs the younger; masses sum exactly; deliberately
**not** an `EatEvent` — a merge is the player's own mass reassembling, not a meal, and it
can never be a death (the owner keeps the elder). Note the ratio gate (100 ≥ 1.25·25)
would make this pair a meal between rivals; the shared owner is what turns eating into
merging.

**Push-apart runs only WHILE a cooldown runs.** During a cooldown, an overlapping pair
(dist < r_a + r_b) gets full positional correction — half the penetration each, along the
centre axis (exactly coincident centres, a zero-intent split, separate along +x, the
deterministic tiebreak), clamped back into the world square. Once both timers expire the
pair **overlaps freely**, and that free overlap is load-bearing: the merge window demands
deep overlap (0.25·max(r_a, r_b) is always far inside touching distance r_a + r_b), so an
unconditional correction would park post-cooldown siblings at exactly touching — forever
outside the window — and make steered remerging unreachable. Merge is checked *before*
push-apart because it is the stricter test: checking push-apart first would shove a pair
apart on the very tick it earned the merge (`Merge.GatedByCooldownThenElderAbsorbsYounger`).

**Same-owner resolution is all-pairs per player, not grid-driven** — the documented
exception to invariant 6: the ≤ 16 cap makes O(k²) free, and two giant siblings can rest
centre-to-centre far beyond `grid_cell_size` — past the candidate-pair walk's completeness
limit — so the grid would silently miss exactly the pairs this phase exists for. Pairs
resolve in (owner-major, index-minor) sorted order: the same array-order tiebreak doctrine
as eating.

## The eject ledger

Eject (per sufficiently heavy cell, on a latched one-shot flag) is a strict mass ledger:

```
cell pays        eject_mass_cost  = 18
pellet carries   ejected_mass     = 14
evaporates       cost − carried   =  4       — ejecting must never print mass
```

Without the evaporation gap, eject-and-eat-it-back would be a free mass pump; the config
validator enforces the whole ordering `ejected_mass ≤ eject_mass_cost ≤ min_eject_mass` —
no printing, and a minimum-mass cell survives its own eject. Gates: `mass ≥
min_eject_mass` (35) **and** a non-zero direction — "hold still" has no aim, so nothing
fires and no cost is paid. Conservation is pinned with `==`
(`Eject.PaysTheCostAndTheDifferenceEvaporates`).

**Rim-to-rim placement**, derived from existing knobs rather than a new margin:

```
birth = cell.position + dir · ( r(mass − cost) + r(ejected_mass) )
```

— tangent to the (post-cost) parent, fully outside it, so this tick's inclusive eat pass
cannot instantly re-eat it; and it only ever moves further away, since `eject_speed`
(1400 u/s) outruns any cell. Worked, a 100-mass cell: r(82) ≈ 36.22 plus r(14) ≈ 14.97 →
the pellet materializes ≈ 51.2 units out. It keeps its **owner** — you may eat your own
ejecta back (at the 4-unit loss), and viruses attribute feeds by owner. Its flight is
`velocity = dir·1400`, decaying by the glide above.

## Viruses

**Field maintenance** mirrors pellets: `step()` refills to `target_virus_count` (40) from
the injected PRNG — pellet draws first, then virus draws, a fixed order the replay depends
on. Refill only ever **adds**: a feed-split can push the population above target, and the
surplus stands — despawning to correct it would yank live terrain out from under the
players hiding on it (`VirusField.RefillsToTheTargetAndNeverCulls`).

**The pop gates are cell-vs-cell's gates aimed at a virus** ([above](#eat-geometry)):
`m_cell ≥ 1.25 · virus_mass` (= 125 at the defaults) and `dist ≤ r_cell − ⅓·r_virus`.
Worked (`VirusPop.NeedsBothTheMassRatioAndTheDepth`): a mass-300 cell has reach
4·√300 − ⅓·40 ≈ **55.95** against a default virus (r 40) — a virus centre at 56.5 stands
untouched (well inside the r_e ≈ 69.3 query circle; the depth rule is what saves it), at
55.4 it pops. Below either gate the virus is **terrain**: a small cell can sit dead-centre
on one for fifty ticks and nothing whatsoever happens
(`VirusPop.BelowGateCellSitsOnAVirusForeverUntouched`) — viruses punish only the big,
which is the anti-snowball intent.

**A pop is a real meal, then a forced split.** The eat is recorded like any other
(`EatEvent`; the virus's mass lands on the eater *before* the burst divides it — mass
moves, never vanishes), then the cell bursts into

```
N = min(virus_pop_pieces, max_cells_per_player − owned + 1)     owned counted at burst time
```

equal shares — worked: 300 + 100 = 400 over N = min(8, 16 − 1 + 1) = 8 → **50.0 each**, a
power-of-two division compared with `==`. Counting `owned` at burst time means
back-to-back pops in one tick shrink each other's bursts, so the cap is never breached; at
15 cells N = 2 (400 → 200 + 200); **at the cap N = 1** — the mass lands, nothing splits,
and no cooldown is armed for a burst that never was: the anti-snowball still bites the
wallet, just not the roster (`VirusPop.CapLimitsThePiecesAndAtTheCapOnlyTheMassLands`).
Every piece goes through the shared M4 split tail, so all N carry the mass-scaled merge
cooldown.

**The burst is rng-free**: the N − 1 launched pieces get impulses of magnitude
`split_impulse_speed` at fixed radial angles **2πk/N** from the +x axis (the original
keeps piece 0's share in place, no kick). A pop is already pure punishment — its scatter
should be readable and replayable, never a lottery — and invariant 3 stays untouched: no
draw, no divergence (`VirusPop.LaunchesPiecesAtFixedRadialAnglesIdenticallyEveryTime`).

**Feeding** runs right after the eat pass — so a cell racing a virus for the same pellet
resolves to the cell (array-order eaters go first), and a virus popped this tick no longer
feeds. Every alive virus absorbs the alive `EjectedMass` whose centres lie inside its
radius (inclusive boundary, the same convention as food), **with no EatEvent**: feeding is
terraforming, not a meal — nothing above core should react to it. The pellet's mass
vanishes with it, and the virus's own mass stays `virus_mass` throughout: **absorbed feed
mass evaporates**, the same anti-mass-printing rule as eject. Each hit bumps `feed_count`
and records `last_feed_dir` through a deterministic fallback chain — normalized pellet
velocity (its remaining flight) → the pellet→virus axis (a long-resting pellet) → +x
(exactly coincident) — every branch a pure function of state. At `virus_feed_count` hits
(7), the split fires: a **new** virus spawns at the fed one's position with
`impulse = last_feed_dir · eject_speed` — the last feeder aims it, and it glides
≈ 1400/3.5 = **400 units**, the genre's fed-virus lunge — and both counts reset
(`VirusFeed.SevenHitsSplitTheVirusAlongTheLastFeedDirection`).

## Safe spawn

`spawn_player` places the starting cell by **bounded PRNG retries**: up to
`safe_spawn_attempts` (16) draws, and the first whose position has no alive Cell of mass ≥
`safe_spawn_threat_mass` (80) within `safe_spawn_radius` (600) wins. Threats are judged
against the **standing grid** — the world as of the last `step()` — so a threat that moved
since is judged one tick stale; accepted and documented, because placement is a comfort
rule, not a correctness one, and the alternative is a mid-call rebuild. When **no** draw
is safe, the last one stands: bounded work by construction — never an infinite loop on a
crowded map — and spawning into danger beats not spawning at all.

**Why a variable draw count does not break determinism**: retries stop at the first safe
hit, so the number of `world.rng` pulls varies — but it is a **pure function of world
state** (rng state + the last step's grid), so a replay that repeats the same lifecycle
calls in the same order repeats the exact same draws. The tests prove the count from the
generator's own state: a same-seed probe twin pins where the first draw would land, the
real world parks a mass-500 threat exactly there, and the spawn must land ≥ 600 away
while `w.rng` sits more than one placement ahead (each placement draw is two raw pulls,
x then y); and on an 800-unit lattice of threats covering every point of the square, the
generator ends **exactly** `2·safe_spawn_attempts` pulls ahead with the player placed
anyway (`SafeSpawn.RetriesPastAThreatenedDrawDeterministically`,
`SafeSpawn.WhenNoDrawIsSafeTheLastOneStands`).

## The zoom curve

How far a player sees ([`core/src/sim/interest.cpp`](../core/src/sim/interest.cpp)):

```
view_radius(m_total) = view_base + view_mass_factor · √m_total  =  640 + 24·√m    (defaults)
```

| total mass | view radius |
|---|---|
| 0 (mid-respawn) | 640.0 |
| 10 (fresh spawn) | ≈ 715.9 |
| 100 | 880.0 |
| 400 | 1120.0 |

**The √ law is deliberate**: drawn radius is r = k·√m, so the view widens at exactly the
rate the player's own cells grow on screen — quadrupling the mass doubles the mass-driven
part of the view, exactly as it doubles `radius_for_mass`; anything shallower and a big
cell would outgrow its own camera. `view_base` keeps a fresh spawn seeing something, and
`max(mass, 0)` floors the degenerate inputs a session can produce (mass 0 mid-respawn, a
negative from a bug) at `view_base`
(`Interest.ViewRadiusFollowsTheSqrtLawAndIsMonotone`).

The input is the player's **total** cell mass (all pieces summed) and the centre their
mass-weighted centroid, both computed server-side per peer. `collect_visible` answers the
circle through the grid, boundary inclusive, differential-tested against brute force on
uniform and clustered layouts; its output order is the grid's bucket order — a pure
function of the entity array — which is what the per-peer budget's determinism builds on
([protocol.md](protocol.md#per-peer-sending)).

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
`Σ v·dt_i = v·Σ dt_i = v·T` — the slicing cancels. (Decaying velocities don't get this for
free; they use the exact glide integral [above](#the-impulse-glide).) The canary is
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
guarantee ends, which is why anything with a longer reach — a big cell's eat radius, a
view-radius query — must use `for_each_in_circle`, which walks the whole covered cell
rectangle instead. Eat resolution, virus feeding, safe spawn and `collect_visible` all do
exactly that (invariant 6's "documented big-eater path"); same-owner resolution goes one
further and skips the grid entirely — the cap-bounded all-pairs exception derived in
[Split & merge](#split--merge). The candidate-pair walk itself currently has no caller in
`step()`; it remains available for future short-range different-owner mechanics.

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
keeps discovery order — and therefore contested-meal outcomes — replayable. It is also
what makes `collect_visible`'s output order, and hence per-peer snapshot content, a pure
function of the world.

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
- **Everything dt-scaled** — integration linearly, decay and damping exponentially
  (`e^(−λ·dt)`), decaying displacement by the exact glide integral.
- **No rng where a fixed rule serves** — the pop burst's 2πk/N angles, the feed-direction
  fallback chain, and every tiebreak (array order, elder id, +x) are deliberate constants.
- **Lifecycle calls are part of the input sequence**: pellet respawn, virus respawn and
  `spawn_player`'s safe-spawn retries all draw from `world.rng` in a fixed order (pellets
  before viruses; a retry loop's draw count is itself a pure function of world state — see
  [Safe spawn](#safe-spawn)), so a replay must repeat every `spawn_player` /
  `despawn_player` / `apply_intent` / `step` in the same order — the world is a pure
  function of (seed, call sequence).

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

**The enforcement** is a family of replay tests, one per mechanic generation, each run
twice from the same seed and compared with **exact equality** (`EXPECT_EQ`, no tolerance)
of tick, `next_id`, the full PRNG state (`a.rng == b.rng` — the generators advanced in
lockstep) and every entity field the generation added:

- `Replay.SameSeedAndScriptReplayIdentically` (M3): 200 ticks, two players steering on a
  schedule, a planted kill, a live 2000-pellet field, lifecycle churn (despawn at tick
  100, respawn at 120).
- `Replay.SplitEjectMergeReplayIdentically` (M4): 300 ticks adding splits (with refusals),
  ejects, push-apart and a merge (under a script-widened window — the default window plus
  full positional correction cannot be reached in 300 scripted ticks without teleports),
  with the impulse and cooldown fields in the comparison.
- `Replay.VirusPopsFeedsAndChurnReplayIdentically` (M5): 300 ticks over live pellet
  **and** virus fields — a scripted pop, a scripted feed-split, and lifecycle churn now
  through the safe-spawn retry loop, whose draw count is itself part of the replayed
  sequence; the comparison covers the feed fields.

It is the cheapest desync detector the project has and, later, the foundation for
client-side prediction; every new mechanic is expected to extend the family.

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
