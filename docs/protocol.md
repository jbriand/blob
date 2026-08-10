# Protocol

The wire format, byte for byte: five message ids over two ENet channels, all integers
**little-endian**, all packing rules in exactly two files —
[`core/include/blob/net/protocol.hpp`](../core/include/blob/net/protocol.hpp) (+
[`protocol.cpp`](../core/src/net/protocol.cpp)) for message layout and
[`core/include/blob/net/quantize.hpp`](../core/include/blob/net/quantize.hpp) for
float↔wire conversion — shared verbatim by client and server, so a second definition of
the format cannot exist.

## Version policy

`protocol_version` is currently **3** (2: the M1 snapshot family; 3: the Hello payload +
Goodbye reasons — M6's wire changes, batched into one bump). Any packing change — field
added, width changed, encoding changed, `EntityKind` extended — bumps it; wire changes
within one iteration batch into a single bump. The test
`Snapshot.ProtocolVersionPinnedAtThree` re-pins the literal so an accidental format change
fails loudly and a deliberate one is re-pinned consciously.

Refusal is **symmetric since v3**. Hello carries the client's version, and the *server*
refuses a mismatch first — a reliable `Goodbye{VersionMismatch}`, then the hang-up, before
spawning anything ([session flow](#session-flow)). Welcome still carries the server's
version and the client still checks it on arrival, belt-and-braces: a Welcome that somehow
slipped past a mismatched handshake is caught one step later. (Before v3 the check was
client-side only — the client sent no version, so the server had nothing to judge.)

## Channels

| Channel | Value | ENet mode | Carries | Why |
|---|---|---|---|---|
| `Control` | 0 | reliable, ordered (`ENET_PACKET_FLAG_RELIABLE`) | Hello ↑, Welcome ↓, Goodbye ↓ | session state must arrive, in order |
| `Snapshot` | 1 | unreliable sequenced (flags 0) | Snapshot chunks ↓ **and** Input ↑ | the 20 Hz firehose — a dropped snapshot is always better than a late one (invariant 5) |

Invariant 5's rationale: a snapshot is absolute state that the next tick supersedes, so
retransmitting a lost one delivers stale data late and delays everything behind it.
**Input rides the unreliable channel for the same reason** — it is a latest-wins stream (a
lost input is outdated by the next one, 50 ms later) and the u16 sequence guard
([below](#the-sequence-guard)) makes reordering harmless. Reliability would only add
head-of-line blocking to data whose value expires faster than a retransmit.

Flags 0 in ENet means **unreliable sequenced**: within the channel, a packet that arrives
after a later-sent one is dropped by ENet itself. For snapshot chunks that is exactly
right — a tick-T chunk arriving after a tick-T+1 chunk is already superseded — and the
client's own latest-tick guard makes the same call one layer up.

## Message catalog

| Id | Byte | Direction | Size | Codec |
|---|---|---|---|---|
| `Hello` | `0x01` | client → server | 4 + n B (n = name bytes, ≤ 16) | `write_hello` / `read_hello` |
| `Input` | `0x02` | client → server | 6 B | `write_input` / `read_input` |
| `Welcome` | `0x81` | server → client | 8 B | `write_welcome` / `read_welcome` |
| `Snapshot` | `0x82` | server → client | 7 + n·13 B | `write_snapshot` / `read_snapshot` |
| `Goodbye` | `0x83` | server → client | 2 B | `write_goodbye` / `read_goodbye` |

### Hello — 4 + n bytes

The client's introduction, sent reliably on channel 0 the moment the transport comes up:
the version the server will judge, plus a display-only nickname (the sim never sees
names).

| Offset | Field | Type | Encoding |
|---|---|---|---|
| 0 | message id | u8 | `0x01` |
| 1 | `version` | u16 | the client's `protocol_version`; the server refuses a mismatch |
| 3 | `name_len` | u8 | name bytes that follow; ≤ `max_hello_name_bytes` = 16 |
| 4 | `name` | n × u8 | UTF-8, **not** NUL-terminated |

Length discipline is split between the sides on purpose. `write_hello` treats a name over
16 bytes as **flagged misuse** — the overflow flag goes up and nothing at all is written,
mirroring `write_snapshot`'s oversized-span rule — because truncation is a UI decision,
never the codec's (the client truncates at the door before calling, and cutting
mid-UTF-8-sequence is harmless for a display-only string). `read_hello` rejects a
`name_len` over the cap (it cannot come from `write_hello`) **and** a `name_len` claiming
bytes the buffer does not contain, before copying anything. `HelloPayload` carries the
name as a fixed `std::array<char, 16>`, not a `std::string` — codecs never allocate
(invariant 7's spirit).

### Input — 6 bytes

| Offset | Field | Type | Encoding |
|---|---|---|---|
| 0 | message id | u8 | `0x02` |
| 1 | `sequence` | u16 | wrapping serial, client-incremented per send |
| 3 | `dir_x` | i8 | `quantize_direction` of a unit vector's x |
| 4 | `dir_y` | i8 | `quantize_direction` |
| 5 | flags | u8 | bit 0 = `split`, bit 1 = `eject`, rest ignored |

The flags are **edges, not levels**: the client sets them on a key-press only (one press
rides exactly one Input), and the server OR-latches them per session so a press cannot be
erased by later steering-only Inputs — one press, one action, however the datagrams
interleave ([architecture.md](architecture.md#the-server-loop)).

### Welcome — 8 bytes

| Offset | Field | Type | Encoding |
|---|---|---|---|
| 0 | message id | u8 | `0x81` |
| 1 | `version` | u16 | the server's `protocol_version`; the client's belt-and-braces check |
| 3 | `player_id` | u16 | never 0 |
| 5 | `world_extent` | u16 | whole world units; the client's dequantization denominator |
| 7 | `tick_rate` | u8 | Hz; the client's input-send cadence |

The config validator pins these widths at the source: `tick_rate` must sit in [1, 255]
and `world_extent` in (0, 65535] or the server refuses to start — anything else would
silently truncate here.

### Goodbye — 2 bytes

Why the server is dropping the peer, sent reliably on channel 0 before the hang-up
(`enet_peer_disconnect_later`, so the reason actually flushes before the teardown).

| Offset | Field | Type | Encoding |
|---|---|---|---|
| 0 | message id | u8 | `0x83` |
| 1 | `reason` | u8 | 1 = `VersionMismatch`, 2 = `ServerFull`, 3 = `Shutdown` — **0 is deliberately unused** |

Zero is not a reason so that a zeroed buffer can never decode as a valid Goodbye, and
`read_goodbye` rejects by **whitelist, not range check** — an unknown byte is malformed
wire data, never a "misc" bucket, and a reason removed from the enum some day starts being
rejected without the check changing shape. Today the shipped server sends only
`VersionMismatch`; `ServerFull` and `Shutdown` are wire-defined — and the client decodes
and prints all three human-readably — but no server code path produces them yet.

### Snapshot chunk — 7 + n·13 bytes

Header, 7 B:

| Offset | Field | Type | Encoding |
|---|---|---|---|
| 0 | message id | u8 | `0x82` |
| 1 | `tick` | u32 | the u64 sim tick truncated — 2³² ticks at 20 Hz ≈ **6.8 years** of uptime before wrap, fine |
| 5 | `count` | u16 | records in **this chunk** (≤ 91), not in the world |

Then `count` × `EntityRecord`, 13 B each:

| Offset | Field | Type | Encoding |
|---|---|---|---|
| +0 | `id` | u32 | `EntityId` (never 0) |
| +4 | `owner` | u16 | `PlayerId`, 0 = unowned |
| +6 | `kind` | u8 | `EntityKind` mirror; must be < 4 |
| +7 | `x` | u16 | `quantize_position(x, extent)` |
| +9 | `y` | u16 | `quantize_position(y, extent)` |
| +11 | `mass` | u16 | `quantize_mass` — linear whole units, saturating |

Byte order is a wire-format promise, pinned before a second implementation exists:
`Protocol.WideWritesAreLittleEndianOnTheWire` asserts `write_u32(0x11223344)` lands as
`44 33 22 11`.

## Chunking

A pellet field is far bigger than one datagram, so chunking exists from day one:

```
snapshot_soft_mtu        = 1200 B         (comfortably under the common 1500 B path MTU,
                                           with room for IP/UDP/ENet framing — a chunk
                                           never fragments)
max_entities_per_chunk   = (1200 − 7) / 13 = 91      (1193 / 13 = 91.77, floored)

full chunk               = 7 + 91·13 = 1190 ≤ 1200   ✓
one more record          = 7 + 92·13 = 1203 > 1200   ✗
```

Both edges are pinned in `Snapshot.ChunkArithmeticFitsTheMtuBudget`. The shipped slicing is
`for_each_chunk` in [`server/src/snapshot_encode.hpp`](../server/src/snapshot_encode.hpp):
subspans of ≤ 91 in array order — 200 records make chunks of 91 + 91 + 18 — and **zero
records make zero chunks** (an empty view is not worth a datagram, though a 7-byte empty
chunk is perfectly legal at the codec level and `Snapshot.EmptySnapshotIsLegalAndExactlySevenBytes`
proves it).

**Every chunk is self-contained**: it repeats the tick and carries its own count, so no
chunk depends on any other. The client's assembly rule (`apply_chunk`, mirrored exactly in
the loopback test) is latest-tick-wins:

- chunk tick **newer** than the view → *replace* the entity set, adopt the tick;
- chunk tick **equal** → *append* (another chunk of the same snapshot);
- chunk tick **older** → *drop* (it lost the race; the newer state already superseded it).

Losing a chunk is benign **because records are absolute state**: partial application just
means some entities are missing from one frame and pop back with the next snapshot, 50 ms
later. There is no delta to corrupt — that trade changes if delta snapshots ever land
(deferred to M7, when measurements demand them; `InputCommand::sequence` is the ack
groundwork already in place).

## Per-peer sending

Since M6 the server does not broadcast: **each peer gets its own snapshot**, cut to what
that player can see ([`server/src/main.cpp`](../server/src/main.cpp) +
[`snapshot_encode.cpp`](../server/src/snapshot_encode.cpp)). Per session, per send:

1. **View centre** = the mass-weighted centroid of the player's cells (held from the last
   send while the player briefly has none, mid-respawn).
2. **View radius** = `view_base + view_mass_factor·√(total cell mass)` — the zoom curve,
   derived in [simulation-math.md](simulation-math.md#the-zoom-curve).
3. **Visible set** = everything within that circle, answered by the grid
   (`sim::collect_visible`), then encoded — unless it exceeds the budget:
4. **Budget** = `snapshot_chunks_per_tick × max_entities_per_chunk` records (default
   3 × 91 = **273**). An over-budget view keeps exactly the **nearest** entities — sort key
   (dist² to centre, then index ascending), a fully deterministic order, because what a
   peer receives must be a pure function of the world, never of allocator or hash
   accidents (`SnapshotEncode.OverBudgetKeepsExactlyTheNearestRecords`,
   `…BudgetDistanceTiesBreakOnIndexAscending`, `…PerPeerSelectionIsDeterministic`). The far
   rim of the view is the part whose absence a player notices least.

The budget lives in **`ServerConfig`, not `Tuning`** — it is operational (like `port`), not
gameplay: it shapes datagram spend per peer, never what the simulation does, so it has no
business in the replayed-input aggregate. The point of the cap is that one crowded view
cannot monopolize the uplink: worst case per peer is 3 × 1190 B × 20 Hz = **71.4 kB/s**
regardless of world size. Measured effect on a 2-player 2000-pellet world: full-world
broadcast cost 523.6 kB/s per peer; a spawn-sized view costs ~12.9 kB/s — about 40× less —
and the full-world-broadcast cliff is retired. Peers that have not completed the Hello
handshake get no snapshots at all ([session flow](#session-flow)).

## Robustness contract

Readers return `std::nullopt` on **any** malformation, writers set a sticky `overflowed`
flag and never write past the buffer; neither ever throws (invariant 7). The cases, each
pinned by a test in [`core/tests/test_snapshot.cpp`](../core/tests/test_snapshot.cpp) /
[`test_quantize.cpp`](../core/tests/test_quantize.cpp):

| Malformation | Result | Test |
|---|---|---|
| wrong message-id byte | nullopt | `Snapshot.WrongMessageIdIsRejected`, `Hello.WrongMessageIdIsRejected`, `Goodbye.TruncationAndWrongMessageIdAreRejected` |
| truncation anywhere — every proper prefix of a 3-record chunk (46), an 11-byte Hello, a Goodbye | nullopt | `Snapshot.EveryTruncationIsRejectedNotGuessed`, `Hello.EveryTruncationIsRejectedNotGuessed`, `Goodbye.TruncationAndWrongMessageIdAreRejected` |
| garbage kind (≥ 4) | nullopt | `Snapshot.OutOfRangeKindIsRejected` |
| count claiming records the bytes don't contain | nullopt | `Snapshot.CountLyingBeyondTheBytesIsRejected` |
| count > 91, even with the records genuinely present | nullopt | `Snapshot.CountAboveChunkLimitIsRejected` |
| count > caller's `out` span | nullopt — reject, never spill | `Snapshot.UndersizedOutSpanIsRejected` |
| `name_len` > 16, even with the bytes genuinely present | nullopt | `Hello.NameLengthAboveTheCapIsRejectedEvenWithTheBytesPresent` |
| `name_len` claiming bytes the buffer doesn't contain | nullopt | `Hello.EveryTruncationIsRejectedNotGuessed` |
| Goodbye reason outside {1, 2, 3} — including 0, the zeroed-buffer trap | nullopt | `Goodbye.UnknownReasonIsRejected` |
| writer out of room | flag; a straddling write lands what fits, nothing past the end | `Snapshot.WriterOverflowSetsFlagInsteadOfScribbling` |
| `write_snapshot` span > 91 records | **flagged misuse, nothing written at all** — the check runs before the first byte | `Snapshot.OversizedSpanIsFlaggedMisuseNotAScribble` |
| `write_hello` name > 16 bytes | flagged misuse, nothing written — same rule | `Hello.OversizedNameIsFlaggedMisuseNotTruncated` |
| corrupted public cursor (`offset` = SIZE_MAX) | flag, no wrap, no stray access | `Protocol.MaximalOffsetCannotWrapPastTheBoundsCheck` |

On success, `read_snapshot` fills the first `count` entries of `out` and returns the
header. The `offset >= size` bounds-check idiom that makes the last row work is explained
in [data-structures.md](data-structures.md#bytewriter--bytereader). The server's stance on
a rejected packet is *drop and move on* — malformed wire data is never fatal, and on a
half-open session (no Hello yet) everything that is not a valid Hello is dropped the same
way.

## Quantization

The snapshot stream is the bandwidth bill — every visible entity × 20 Hz × every peer — so
every field crossing the wire is quantized, and **wire values are display-only**: the
server keeps the authoritative floats and resolves all gameplay against them, so
quantization error is a rendering concern, never a correctness one. (The README's
[Quantization section](../README.md#quantization) tells the same story with the full
mass-encoding rationale.)

| Field | Encoding | Step | Worst error (round-to-nearest = half a step) |
|---|---|---|---|
| position | u16 per axis: `round(clamp(v/extent, 0, 1) · 65535)` | extent/65535 ≈ **0.125 units** at 8192 | extent/65535/2 ≈ **0.0625 units** |
| direction | i8 per axis: `round(clamp(v, −1, 1) · 127)` | 1/127 ≈ 0.0079 | ≈ 0.39 % per axis |
| mass | u16: `round(clamp(m, 0, 65535))` | 1 unit | **0.5 units** |

Worked example, position: `x = 123.456` at extent 8192 → `123.456/8192 · 65535 + 0.5` →
code `988` → decodes to `988/65535 · 8192 ≈ 123.502`, error ≈ 0.046 < 0.0625. Out-of-range
values clamp, never wrap (`Quantize.ValuesAreClampedNotWrapped`); the end-to-end bound —
through `write_snapshot`/`read_snapshot` with the real quantizers — is pinned by
`Snapshot.QuantizedFieldsStayWithinErrorBounds`.

**Why 8 bits suffice for direction**: it encodes a unit vector's components for a server
that re-normalizes on receipt and clamps speed by mass anyway — sub-degree steering
precision would be spent on nothing. `quantize_direction` rounds away from zero and so
never produces −128; `dequantize_direction` clamps `q/127` into [−1, 1], so a hostile
−128 decodes to −1.0 rather than −1.008.

**Why mass is linear** rather than the byte-ranged `√mass` a bandwidth purist would reach
for: drawn radius is `r = k·√m`, so packing `√m` gives uniform steps in *radius* — but its
representable masses are roughly the perfect squares (1, 4, 9, 16, 25…), which destroys
exactly the small end that must be exact: a HUD score decoded from them jumps visibly and
pellet masses collapse onto a couple of codes. Linear u16 keeps every mass the game
currently produces exact on the wire (±0.5) and saturates at 65 535. If per-entity bytes
ever become the *measured* bottleneck: per-kind records first (pellets need neither `mass`
nor `owner`), √mass second — either bumps `protocol_version`.

## The sequence guard

The input stream is unreliable *and* unordered, so the server applies an Input only if its
sequence is strictly newer than the last applied one — serial-number arithmetic on the u16
circle ([`server/src/session.hpp`](../server/src/session.hpp)):

```cpp
sequence_newer(a, b)  =  int16_t(uint16_t(a − b)) > 0
```

The subtraction is exact mod 2¹⁶; reinterpreting it as signed asks "is `a` less than half
the circle ahead of `b`?". Edge cases, all pinned in
[`server/tests/test_session.cpp`](../server/tests/test_session.cpp):

| a | b | `a − b` (mod 2¹⁶) | as i16 | newer? |
|---|---|---|---|---|
| 1 | 0 | 1 | +1 | yes |
| 0 | 1 | 65535 | −1 | no |
| 0 | 65535 | 1 | +1 | **yes — the wrap case**: 0 follows 65535, a long session never freezes |
| 65535 | 65535 | 0 | 0 | no — equal is never newer, a duplicated datagram must not re-apply |
| 32767 | 0 | 32767 | +32767 | yes — the farthest "newer" |
| 32768 | 0 | 32768 | −32768 | **no** — exactly half the circle apart is ambiguous by construction and lands on "not newer" both ways: the safe side for an input guard |

## Session flow

The handshake is **half-open until a valid Hello**: connect buys a session slot and
nothing else — no spawn, no Welcome, no snapshots — because the version check must pass
before the world spends anything on the peer. A client that never says Hello is reaped by
ENet's own connection timeout (a dedicated Hello deadline is a known simplification,
deferred until it is a measured problem).

```mermaid
sequenceDiagram
    participant C as client
    participant S as server
    C->>S: ENet connect
    Note over S: PlayerId = next_player_id++<br/>add_session — half-open:<br/>no spawn, no Welcome yet
    C->>S: Hello v3 — version, nickname (ch 0, reliable, 4-20 B)
    alt version matches
        Note over S: store nickname, spawn_player
        S->>C: Welcome v3 — player_id, extent, tick_rate (ch 0, reliable)
        loop every 1/tick_rate s (accumulator-paced)
            C--)S: Input seq n (ch 1, unreliable, 6 B)
            Note over S: read_input, sequence guard,<br/>dequantize + re-normalize,<br/>OR-latch split/eject, apply_intent
        end
        loop every server tick (once per loop iteration)
            S--)C: Snapshot chunk tick T, count k (ch 1, unreliable, per-peer visible set)
            S--)C: ... more chunks, same tick, up to 91 records each
            Note over C: newer tick replaces, equal appends, older drops
        end
        C->>S: ENet disconnect
        Note over S: remove_session, despawn_player<br/>(disconnect is not death - no event)
    else version mismatch
        S->>C: Goodbye reason VersionMismatch (ch 0, reliable, 2 B)
        Note over S: disconnect_later - the reason<br/>flushes before the teardown;<br/>the refused peer never spawned
        Note over C: print the reason, exit
    end
```

Both arms run end-to-end over a real UDP socket on 127.0.0.1 in
[`server/tests/test_loopback.cpp`](../server/tests/test_loopback.cpp):
`Loopback.CursorChaseOverRealUdp` (connect → Hello → Welcome → Input → `apply_intent` →
`step` → per-peer snapshot → client decode) and
`Loopback.VersionMismatchIsRefusedWithGoodbye` (the refusal, the flush-before-hang-up, and
that a refused peer never spawns).
