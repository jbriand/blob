# Data structures

Every struct that matters, field by field, with the invariants that hold on it. All of them
follow the plain-struct + free-function convention
([architecture.md](architecture.md#plain-structs--free-functions)): public fields, NSDMI
defaults, aggregates asserted at build time.

**Id semantics, everywhere:** `EntityId` is a `u32`, monotonic from 1, **never reused** —
`0` means "no entity". `PlayerId` is a `u16`, assigned monotonically per server run
starting at 1 — `0` means "no owner" (pellets, viruses). Compaction reuses *indices*, never
ids: nothing may hold an index across a `step()`; hold the id and look it up.

## Vec2

[`core/include/blob/math/vec2.hpp`](../core/include/blob/math/vec2.hpp)

| Field | Type | Meaning | Invariants |
|---|---|---|---|
| `x`, `y` | `float` | 2-D vector / point, world units | `sizeof(Vec2) == 8`, aggregate, trivially copyable — all `static_assert`ed in [`vec2.cpp`](../core/src/math/vec2.cpp) |

Operations live beside the struct as constexpr free functions: `+ − * += −= *=` (non-member
operators), `==` (written out because a defaulted one must be a member or friend; `!=` is
synthesized), `dot`, `length_sq`, `length`, and `normalized`. `normalized` is **zero-safe**:
a vector of length ≤ 1e-6 returns `{0,0}` instead of NaN, because the simulation feeds
cursor deltas straight into it and a cursor exactly on top of the cell is normal input,
not an error.

## Entity

[`core/include/blob/sim/world.hpp`](../core/include/blob/sim/world.hpp)

| Field | Type | Meaning | Invariants |
|---|---|---|---|
| `id` | `EntityId` | identity | monotonic, never reused, ≥ 1 |
| `owner` | `PlayerId` | owning player | 0 for unowned kinds (Pellet, Virus); EjectedMass **keeps its ejector's id** — you may eat your own ejecta back, and viruses attribute feeds by owner |
| `kind` | `EntityKind` | `Cell` 0, `Pellet` 1, `Virus` 2, `EjectedMass` 3 | numeric values are a wire promise (see the mirror, below) |
| `position` | `Vec2` | authoritative floats | clamped to `[0, world_extent]` per axis by `step()` |
| `velocity` | `Vec2` | world units/s | Cells: rewritten from intent each step. EjectedMass: its whole flight lives here (no intent to overwrite it) and decays by `e^(−λ·dt)` |
| `impulse` | `Vec2` | decaying launch velocity — the split/pop kick, a fed virus's lunge | a **separate field on purpose**: intent overwrites `velocity` every tick, so a kick stored there would vanish after one step; *added* during integration via the exact glide integral ([simulation-math.md](simulation-math.md#the-impulse-glide)) |
| `mass` | `float` | authoritative mass | transfers on eat and merge, never vanishes — except the two deliberate evaporations (eject's cost − carried, virus feed) |
| `merge_cooldown` | `float` | seconds until this Cell may merge with a same-owner sibling | armed by split/pop (base + per-mass scale), dt-decremented, **floored by assignment at exactly 0.0f** — the merge gate tests `== 0` |
| `feed_count` | `uint8_t` | ejected-mass hits this Virus absorbed since its last feed-split | meaningful only while `kind == Virus`; reset to 0 by the split |
| `last_feed_dir` | `Vec2` | direction of the most recent feed | the launch direction of the split a full count fires — the last feeder aims the virus |
| `dead` | `bool` | intra-step tombstone | see lifecycle below |

**The `dead` flag's lifecycle** is strictly inside one `step()`: eat resolution (and
same-owner merges, and virus feeding) *marks* victims dead — removal would misalign entity
indices with the grid built over this tick's positions — later phases skip dead entities,
and the end-of-step compaction (`std::erase_if`) removes them **before `step()` returns**.
So `dead` is never `true` between steps.

**None of the state fields cross the wire.** `dead` (M3), `impulse` + `merge_cooldown`
(M4) and `feed_count` + `last_feed_dir` (M5) are server-side only: `EntityRecord` still
carries exactly id/owner/kind/x/y/mass, and none of those iterations touched it — the v3
bump was the Hello payload and Goodbye reasons (M6), not the record. Timers and kicks are
simulation state the client reconstructs visually from positions alone; sending them would
buy nothing and cost 20 Hz × every visible entity.

## PlayerIntent

| Field | Type | Meaning | Invariants |
|---|---|---|---|
| `player` | `PlayerId` | whose intent | one intent per player: `apply_intent` upserts |
| `direction` | `Vec2` | unit vector, or `{0,0}` for "hold still" | server-side it has been re-normalized at the door; a **level** — it persists tick to tick |
| `split`, `eject` | `bool` | one-shot action flags | **edges, not levels**: `step()`'s action phase performs, then clears, so one latched press yields exactly one action (split runs before eject, a fixed order) |

## EatEvent / StepEvents

| Struct | Field | Type | Meaning |
|---|---|---|---|
| `EatEvent` | `eater`, `eaten` | `EntityId` | one meal, in resolution order — including virus pops (a pop *is* a meal, `eaten` = the virus) |
| `StepEvents` | `eats` | `vector<EatEvent>` | every meal this step. **Not** recorded: same-owner merges (the player's own mass reassembling) and virus feeds (terraforming) — nothing above core should react to either |
| | `deaths` | `vector<PlayerId>` | players whose **last** Cell fell this step — sorted-unique (derived by `set_difference` over owners-before vs owners-after), so a player losing every cell in one tick dies exactly once |

`StepEvents` is deliberately a **field on `World`, not a `step()` return value**: callers
that ignore events keep calling `step(world, dt)` unchanged (data over plumbing — a return
type would have churned a signature a parallel branch was calling), and the vectors'
capacity is reused tick to tick. The price is a contract: **events describe the most recent
step only and are cleared at the top of the next one** — consumers must read them before
stepping again, which is exactly why the server consumes deaths per step inside its
catch-up loop ([architecture.md](architecture.md#the-server-loop)).

## World

[`core/include/blob/sim/world.hpp`](../core/include/blob/sim/world.hpp) — "aggregate,
deterministic state" means: every field below is either replayed input, a pure function of
the call sequence, or reusable scratch whose contents never influence results.

| Field | Type | Meaning / determinism role |
|---|---|---|
| `entities` | `vector<Entity>` | array order = spawn order, preserved by the *stable* compaction — and array order **is** the eat-resolution tie-break, so order is load-bearing for determinism |
| `intents` | `vector<PlayerIntent>` | latest intent per player; part of the replayed input |
| `next_id` | `EntityId` `{1}` | monotonic id source; pure function of the call sequence |
| `tick` | `uint64_t` | incremented once per `step()`; truncated to u32 on the wire |
| `tuning` | `Tuning` | every gameplay constant (below); part of the replayed input |
| `grid` | `SpatialGrid` | broad phase, rebuilt by `step()` — **derived scratch, never authoritative**; standing contract: it describes the world *as of the last `step()`* |
| `rng` | `std::mt19937` | the injected PRNG invariant 3 demands: seed is part of the replayed input, and every consumer — pellet respawn, virus respawn, `spawn_player`'s safe-spawn retries — draws from here and nowhere else, in a fixed order |
| `events` | `StepEvents` | what the last `step()` did; cleared by the next |
| `owners_before_scratch`, `owners_after_scratch` | `vector<PlayerId>` | `step()`-internal scratch kept as fields purely for capacity reuse; contents mean nothing outside `step()` |
| `cell_scratch` | `vector<uint32_t>` | same contract: alive-Cell indices grouped by owner, rebuilt by same-owner resolution each step |

Lifecycle free functions: `make_world(seed)` (default `World{}` stays legal — default-seeded
rng, what most movement tests use), `spawn`, `apply_intent`, `spawn_player` (one Cell of
`spawn_mass`, placed by the M5 safe-spawn rule — bounded PRNG retries against threat cells,
[simulation-math.md](simulation-math.md#safe-spawn) — **consuming `world.rng`, which makes
lifecycle calls part of the deterministic input sequence**), `despawn_player` (immediate
removal of the player's entities *and standing intent* — a respawn under a recycled
PlayerId must not inherit a ghost cursor; disconnect is not death, no event fires), and
`step(world, dt)`.

## Tuning

[`core/include/blob/sim/tuning.hpp`](../core/include/blob/sim/tuning.hpp) — every gameplay
constant as *data* in one aggregate (not scattered `constexpr`), precisely so the server
can override values at startup while `core` stays I/O-free. `default_tuning` is what ships
and what the tests pin against.

| Knob | Default | Meaning |
|---|---|---|
| `tick_rate` | `20` | simulation Hz; server-authoritative, announced in Welcome, never negotiated |
| `world_extent` | `8192.0f` | side of the square world — doubles as the denominator of position quantization |
| `base_speed` | `720.0f` | speed of a mass-10 cell, units/s — the anchor of the speed curve; tune for game pace |
| `speed_mass_exponent` | `-0.44f` | mass→speed falloff; **must stay negative** or the biggest cell is also the fastest |
| `radius_factor` | `4.0f` | `r = radius_factor·√mass` |
| `grid_cell_size` | `256.0f` | broad-phase bucket side; must stay ≥ the largest pair-interaction distance ([simulation-math.md](simulation-math.md#the-spatial-grid)) |
| `eat_ratio` | `1.25f` | cell-vs-cell (and cell-vs-virus) mass gate, strictly > 1 — near-equal cells must not eat on touch |
| `eat_depth_factor` | `1/3.0f` | victim centre must sit inside the eater by this fraction of the victim's radius |
| `target_pellet_count` | `2000` | pellet population `step()` maintains; map density, not an economy |
| `pellet_mass` | `1.0f` | exactly 1 keeps early growth countable, and the linear wire encoding shows it exactly |
| `spawn_mass` | `10.0f` | fresh player's starting Cell |
| `decay_threshold` | `200.0f` | decay taxes only mass above this line and floors at it — starvation deaths are impossible |
| `decay_rate` | `0.002f` | λ per second, applied as `e^(−λ·dt)` (invariant 3) |
| **M4 — split / eject / merge** | | |
| `min_split_mass` | `36.0f` | a cell splits only at or above this, so each half stays viable rather than instant food |
| `max_cells_per_player` | `16` | the classic hard cap — also what keeps a virus pop punishing instead of infinite |
| `split_impulse_speed` | `780.0f` | launch speed of the split half along the intent, units/s; decays via `impulse_damping_rate` |
| `impulse_damping_rate` | `3.5f` | exponential damping λ for `Entity.impulse` and EjectedMass flight — `e^(−λ·dt)` form, same doctrine as `decay_rate` |
| `merge_cooldown_base` | `10.0f` | merge cooldown = base + per_mass · mass-at-split, seconds |
| `merge_cooldown_per_mass` | `0.02f` | the mass scale: big splits stay committed longer |
| `merge_overlap` | `0.25f` | same-owner cells merge within `merge_overlap · max(r_a, r_b)`, once both cooldowns expire |
| `min_eject_mass` | `35.0f` | below this a cell cannot eject at all |
| `eject_mass_cost` | `18.0f` | what the cell pays per eject |
| `ejected_mass` | `14.0f` | what the pellet carries — the cost−carried difference **evaporates**: ejecting must never print mass |
| `eject_speed` | `1400.0f` | launch speed of the ejected pellet, units/s; damped by `impulse_damping_rate` |
| **M5 — viruses & safe spawn** | | |
| `target_virus_count` | `40` | virus population, refilled like pellets — but refill only ever *adds* |
| `virus_mass` | `100.0f` | mass of a fresh virus, and the reference the pop gate scales from |
| `virus_pop_pieces` | `8` | pieces a pop tries to burst the cell into, capped by `max_cells_per_player` |
| `virus_feed_count` | `7` | ejected-mass hits a virus absorbs before it splits toward the feeder |
| `safe_spawn_radius` | `600.0f` | spawn placement keeps this clear of threat cells |
| `safe_spawn_threat_mass` | `80.0f` | a cell at or above this counts as a spawn threat |
| `safe_spawn_attempts` | `16` | bounded placement retries; the last draw stands regardless — bounded work, deterministic |
| **M6 — interest management** | | |
| `view_base` | `640.0f` | visible radius at zero mass — everyone sees something at spawn |
| `view_mass_factor` | `24.0f` | view radius grows by this · √(total mass), the same √ law as drawn radius |

Derived values are **functions, never stored companions** — `tick_dt(tuning)` and
`radius_for_mass(tuning, mass)` (zero-safe: a negative mass yields radius 0, not NaN) —
so a config override can never leave a stale companion constant behind.

**The config-file story:** the server's flat `key = value` file
([`server/blob-server.cfg.example`](../server/blob-server.cfg.example), parsed by
[`server/src/config.cpp`](../server/src/config.cpp)) overrides every `Tuning` field —
all **33** knobs above, each with a parser branch and a `KeyLines` slot — plus the host
settings `port`, `max_clients` and `snapshot_chunks_per_tick` (36 keys total, every one
round-tripped by `Config.EveryKeyRoundTrips`). Validation pins the wire where it must —
`tick_rate` ∈ [1, 255] (u8 in Welcome), `world_extent` ∈ (0, 65535] (u16 in Welcome) —
and sanity elsewhere: `eat_ratio` > 1, `virus_pop_pieces` ≥ 2 ("a pop that cannot split
is just a meal"), and the eject ledger's ordering `ejected_mass ≤ eject_mass_cost ≤
min_eject_mass` (no mass printing; a minimum-mass cell survives its own eject), among
others.

## SpatialGrid + GridEntry

[`core/include/blob/sim/spatial_grid.hpp`](../core/include/blob/sim/spatial_grid.hpp)

| Struct | Field | Type | Meaning |
|---|---|---|---|
| `GridEntry` | `index` | `uint32_t` | position in the span handed to `rebuild()` |
| | `position` | `Vec2` | copied so hot query loops never touch the entity array |
| `SpatialGrid` | `cell_size` | `float` | bucket side, world units |
| | `cols` | `int32_t` | square grid, `cols × cols` buckets (capped at 4096 against garbage tuning; default 8192/256 = 32) |
| | `starts` | `vector<uint32_t>` | `cols²+1` CSR offsets |
| | `entries` | `vector<GridEntry>` | bucket-sorted copies |

**The CSR layout:** bucket `b` owns `entries[starts[b] .. starts[b+1])`. Worked example —
`cols = 2` (buckets 0..3) and five entities A…E at indices 0…4, landing in buckets
A→0, B→3, C→0, D→2, E→0:

```
counting pass    starts = [0, 3, 0, 1, 1]      (count of bucket b at slot b+1)
prefix sum       starts = [0, 3, 3, 4, 5]      (starts[b] = first entry of bucket b)
placement pass   entries = [A, C, E, D, B]     (input order within each bucket = stable)
rotate right     starts = [0, 3, 3, 4, 5]

bucket 0: entries[0..3) = A, C, E      bucket 2: entries[3..4) = D
bucket 1: entries[3..3) = (empty)      bucket 3: entries[4..5) = B
```

The placement pass uses `starts[b]` as bucket `b`'s write cursor (leaving it holding the
bucket's *end*), and the final rotate-right restores begin offsets without a scratch array —
bucket b's begin is bucket b−1's end. Walking the input span in order keeps every bucket in
input order, so the grid is a **pure function of its input**: same span → bit-identical
grid, which is invariant-3 determinism extended to the broad phase (pinned by
`SpatialGrid.RebuildIsDeterministicAndReusesStorage`).

**Rebuilt from scratch every step** — twice, in fact (post-integration for eat queries,
post-compaction so the standing "grid = the world as of the last step" contract holds).
Everything moves every tick, so a two-pass O(n) counting sort beats incremental
maintenance, and `assign`/`resize` reuse both vectors' capacity — steady state allocates
nothing.

**Index-validity contract:** `GridEntry::index` is a position in the span passed to
`rebuild()` and is valid **only until the next rebuild** — `step()`'s compaction shifts
indices between steps, so nothing may hold one longer (hold the `EntityId`). Queries are
defensive per invariant 7: `is_rebuilt()` gates both query templates (a never-rebuilt or
hand-resized grid reads as empty), `bucket_range()` clamps against a hand-corrupted
`starts`, and `cell_coord()` maps any float — out-of-world, even NaN — into a valid cell.

## Interest queries

[`core/include/blob/sim/interest.hpp`](../core/include/blob/sim/interest.hpp) — no new
data structure, just two pure queries over the grid (this module has no idea sessions or
packets exist; the per-peer budgeting that consumes them lives in `server/`):

- `view_radius(tuning, total_mass)` — the zoom curve
  `view_base + view_mass_factor·√(max(total_mass, 0))`, derived in
  [simulation-math.md](simulation-math.md#the-zoom-curve).
- `collect_visible(world, centre, radius, out)` — clears `out` and fills it with the
  *indices* (into `world.entities`) of every entity within `radius`, boundary inclusive,
  answered by `world.grid`. Output order is the grid's bucket order — deterministic, a
  pure function of the entity array — so per-peer snapshot content never depends on
  anything unordered. The grid's index-validity contract applies unchanged: indices are
  valid only until the next `step()`, and the grid describes the world as of the last one
  (`Interest.OutputOrderIsDeterministicAndTheVectorIsCleared`,
  `Interest.NeverRebuiltWorldYieldsAnEmptyVisibleSet`).

## ByteWriter / ByteReader

[`core/include/blob/net/protocol.hpp`](../core/include/blob/net/protocol.hpp),
[`protocol.cpp`](../core/src/net/protocol.cpp)

| Struct | Field | Type | Meaning |
|---|---|---|---|
| `ByteWriter` | `buffer` | `span<byte>` | caller-owned destination — no allocation, ever |
| | `offset` | `size_t` | write cursor, **public** |
| | `overflowed` | `bool` | sticky flag: a write ran out of room (or `write_snapshot`/`write_hello` misuse) |
| `ByteReader` | `buffer` | `span<const byte>` | source |
| | `offset` | `size_t` | read cursor, **public** |
| | `underflowed` | `bool` | sticky flag: a read ran past the end |

The public-cursor design is the plain-struct rule applied to invariant 7 ("codecs never
throw and never scribble"): the flags and `offset` are maintained exclusively by the
`write_*`/`read_*` free functions, but since the type system no longer forbids outside
writes, **nothing may assume a sane offset**. Concretely:

- Bounds checks are spelled **`offset >= size`, never `offset + 1 > size`** — at
  `offset == SIZE_MAX` the addition wraps to `0`, `0 > size` is false, and the "check"
  would sail straight through into `buffer[SIZE_MAX]`. The `>=` form has no arithmetic to
  wrap. Pinned by `Protocol.MaximalOffsetCannotWrapPastTheBoundsCheck`.
- The accessors **saturate**: `written()` returns at most the buffer, `remaining()` returns
  0 past the end, `exhausted()` is true. A corrupted offset yields a wrong-but-defined
  answer and a flag — never a stray access.
- Every wider write funnels through `write_u8`, so the bounds check lives in exactly one
  place; a multi-byte write that straddles the end lands the bytes that fit and raises the
  flag (readers mirror this through `read_u8`).

## Wire structs

Also in [`protocol.hpp`](../core/include/blob/net/protocol.hpp). These are **raw integers
by decision**: `net` describes the wire, not the simulation, so this header includes no
`<blob/sim/…>` header. The one semantic coupling — `EntityRecord::kind` numerically
mirroring `sim::EntityKind` — is enforced by a `static_assert` in
[`core/tests/test_snapshot.cpp`](../core/tests/test_snapshot.cpp), the one place both
modules link. Byte layouts, sizes and encodings live in [protocol.md](protocol.md).

| Struct | Field | Type | Meaning |
|---|---|---|---|
| `InputCommand` | `sequence` | `uint16_t` | client-incremented serial for the server's latest-wins guard |
| | `dir_x`, `dir_y` | `int8_t` | quantized unit direction toward the cursor |
| | `split`, `eject` | `bool` | packed into one flags byte on the wire; key-press edges, OR-latched server-side |
| `HelloPayload` | `version` | `uint16_t` | the client's `protocol_version`; the server refuses a mismatch with `Goodbye{VersionMismatch}` |
| | `name_len` | `uint8_t` | meaningful leading bytes of `name`; ≤ `max_hello_name_bytes` = 16, validated on read |
| | `name` | `array<char, 16>` | UTF-8 nickname, not NUL-terminated — a fixed array, not a string: **codecs never allocate** |
| `WelcomePayload` | `version` | `uint16_t` | the server's `protocol_version`; the client's belt-and-braces check (the server has already refused mismatches at the Hello) |
| | `player_id` | `uint16_t` | who you are, and how to spot "mine" in snapshots |
| | `world_extent` | `uint16_t` | dequantization denominator — the client must use *this*, never a local constant |
| | `tick_rate` | `uint8_t` | the cadence the client paces its input at |
| `GoodbyeReason` | (enum) | `uint8_t` | `VersionMismatch` 1, `ServerFull` 2, `Shutdown` 3 — **0 deliberately unused** so a zeroed buffer never decodes as a valid reason; readers reject by whitelist |
| `EntityRecord` | `id` | `uint32_t` | `EntityId` |
| | `owner` | `uint16_t` | `PlayerId`; lets the client tell "mine" from "theirs" |
| | `kind` | `uint8_t` | numeric `EntityKind` mirror; readers reject ≥ `entity_kind_count` (4) |
| | `x`, `y` | `uint16_t` | `quantize_position` against the Welcome extent |
| | `mass` | `uint16_t` | `quantize_mass` — linear, whole units |
| `SnapshotHeader` | `tick` | `uint32_t` | u64 sim tick truncated (≈ 6.8 years at 20 Hz) |
| | `count` | `uint16_t` | records in **this chunk**, not in the world |

## PlayerSession

[`server/src/session.hpp`](../server/src/session.hpp) — deliberately ENet-free (the
peer ↔ session link is the PlayerId stashed in ENet's `peer->data` pointer, `main.cpp`'s
business), so the layer unit-tests without sockets.

| Field | Type | Meaning | Invariants |
|---|---|---|---|
| `id` | `PlayerId` | monotonic per run, wrap skips 0 | |
| `last_sequence` | `uint16_t` | highest Input sequence applied | meaningless until `received_input` |
| `received_input` | `bool` | false until the first Input | the first Input is accepted unconditionally; afterwards only `sequence_newer` ones |
| `pending_split`, `pending_eject` | `bool` | the M4 action latches | **OR-latched** from every Input (never assigned — a press must not be erased by a later steering-only Input), injected into exactly one tick's intent by the pre-pump step in `main`, then cleared. The latch, not the stored intent, is where a press waits |
| `received_hello` | `bool` | handshake state | the session is **half-open** until a valid Hello: no spawn, no Welcome, no snapshots — and the only message read before it is the Hello |
| `nickname` | `string` | from the Hello, ≤ 16 bytes | display-only; the sim never sees it |
| `last_centroid` | `Vec2` | mass-weighted centre of the player's cells, from the last snapshot send | the interest-query view centre; **survives brief cell-less moments** (mid-respawn the session queries from where the player last was, with mass 0 → `view_base`) |

Storage is a plain vector with linear `add_session` / `find_session` / `remove_session` —
right at 64 peers, where a map costs more in constants than it saves in asymptotics. The
`find_session` pointer is invalidated by add/remove: use immediately, never across an
event. `sequence_newer` (the u16 wraparound compare) is documented in
[protocol.md](protocol.md#the-sequence-guard).

## ServerConfig

[`server/src/config.hpp`](../server/src/config.hpp)

| Field | Type | Default | Meaning |
|---|---|---|---|
| `port` | `uint16_t` | `7777` | UDP listen port; precedence **CLI port > config file > default** |
| `max_clients` | `size_t` | `64` | ENet peer slots (validated ≥ 1) |
| `snapshot_chunks_per_tick` | `int` | `3` | per-peer snapshot budget, in ≤ 91-record chunks per tick (validated ≥ 1). Lives here rather than in `Tuning` because it is **operational, like `port`, not gameplay**: it shapes datagram spend, never what the simulation does ([protocol.md](protocol.md#per-peer-sending)) |
| `tuning` | `sim::Tuning` | defaults | gameplay overrides live in the same file as host settings — one thing to edit on a game-server box |

The surrounding result types encode the fail-loud contract: `ParseResult` holds a config
(defaults plus whatever parsed cleanly — parsing never fails "halfway") *and* every
`ConfigError` found, each carrying the 1-based line that produced it (validation errors
point at the line that last set the offending key), so `main` prints them **all** in one go
instead of one per restart. `LoadResult`/`LoadStatus` distinguish `Absent` (only ENOENT —
benign at the default path, fatal for an explicit `--config` path) from `Unreadable`
(always fatal: a file that exists but cannot be read must never be mistaken for "no
config"). Parser behavior — `#` comments, CRLF tolerance, last-wins duplicates, unknown key
= error (a silently ignored `base_sped` typo is a debugging trap) — is specified by
[`server/tests/test_config.cpp`](../server/tests/test_config.cpp).

## TickLoop

Covered with its field table and accumulator math in
[architecture.md](architecture.md#fixed-timestep-tickloop); the dt-independence argument is
in [simulation-math.md](simulation-math.md#fixed-timestep).
