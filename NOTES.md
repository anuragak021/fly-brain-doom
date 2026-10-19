# NOTES: design decisions and simplifications

Plain-language log of what was done, why, and the tradeoffs. Updated as work proceeds.

## Honesty statement

This is a connectome **inspired** controller, not a simulation of a fly brain.
Every simplification is listed in the "Simplifications" section below.

## Environment

- Python **3.14.6** venv in `.venv/` (created with `uv`, `.python-version` pins 3.14). The pinned
  `numpy==2.5.4` needs Python >= 3.12, so the system Python 3.10 cannot install `requirements.txt`.
  Dependencies are pinned in `requirements.txt`.
- `neuprint-python` reads the API token from `NEUPRINT_APPLICATION_CREDENTIALS`
  (checked in the library source). The token is never written to any file.
- ViZDoom 1.3.2 ships the standard scenarios (`basic.cfg`, `defend_the_center.cfg`).

## Conventions (fixed now so Python and C cannot disagree)

- **Weight matrix layout: `W[post, pre]`.** The model computes `W @ r`, so row `i` is
  everything flowing INTO neuron `i`. In CSR, one row = one postsynaptic neuron.
  A transpose bug would pass a C-vs-Python test (both sides wrong the same way),
  so this is stated in the export header too.
- **Dale's law:** the sign belongs to the presynaptic neuron, so it is applied per
  column of `W`.

## Simplifications

1. Neuron subset only: 300 neurons chosen by ranked upstream walk from 10 descending neurons (not the whole brain).
2. Forward drive uses DNp36 as a name-matched, unverified stand-in for P9.
3. Weight = raw synapse count (>= 5 per pair, summed over ROIs), then normalized. Synapse size and position ignored.
4. Signs by presynaptic neurotransmitter: acetylcholine +1; GABA and glutamate -1 (glutamate is
   inhibitory via GluCl in many fly circuits but excitatory at some synapses); octopamine and other
   modulators weight 0 (dropped). Neurons below 0.5 prediction confidence would be 'unknown' and dropped.
5. Rate model, not spiking.
5b. Input asymmetry: the subgraph has 9 left and 5 right input neurons, and every input neuron on a
   side receives the same drive. The left side therefore gets more total drive. The decode is
   taken relative to the resting state and the network is not symmetric either, so the rest-relative
   readout is doing some balancing work; a perfectly symmetric network would not need it.
6. Row normalisation (each neuron's total input magnitude = 1) then global scaling so the
   spectral radius of |W| = 0.9. This guarantees no runaway but also discards the absolute
   synapse-count differences between neurons.
7. A constant tonic drive (0.2) is given to every non-input neuron. It is NOT in the connectome.
   Without it the net is silent: the 14 visual inputs alone do not reach the outputs.
8. Features: left/right mean luminance (grey 160x120 buffer, left half vs right half) and the
   horizontal position of the largest enemy taken from ViZDoom's labels buffer. The enemy
   position is game-engine ground truth, not a vision pipeline. Enemy offset to the left drives
   the left input neurons, to the right the right ones (proportional to |offset|).
9. Turn decoding uses the steering outputs relative to their resting state:
   diff = (steerR - steerL) - (rest diff). Positive means turn left. **Sign choice:** measured
   response is that driving one side's visual inputs LOWERS that side's DNa activity
   (ipsilateral suppression). If DNa activity meant "turn toward that side", the agent would
   turn AWAY from enemies. `TURN_SIGN = +1` selects the opposite readout (turn toward the
   stimulus) so the agent approaches; this is a decoding choice made by me, not a connectome fact.
10. Fire is a HEURISTIC, not connectome driven: it fires when an enemy is within 0.15 of screen
    centre. Forward/backward outputs are only logged (defend_the_center has no move buttons).

## Phase B results (Python backend, defend_the_center, 5 episodes, CPU)

- Rate model: tau = 20 ms, Euler dt = 5 ms (alpha 0.25), 6 substeps per decision, float32.
- Measured response with tonic 0.2 and input gain 5: left visual drive changed steerL by -0.099 and
  steerR by +0.011; right drive changed steerR by -0.107 and steerL by -0.012 (mirror symmetric).
- When an enemy was off-centre the controller turned toward it in 40 of 42 frames and away in 0
  (the other 2 were inside the deadzone). Frames are few because episodes are short.
- The 40/42 figure only shows the pipeline is self-consistent after `TURN_SIGN` was chosen to make
  the agent approach. It is NOT independent evidence that the connectome itself aims.
- Kills per episode: 0, 2, 2, 0, 1 (mean 1.0). No baseline comparison yet, so this is not
  evidence the controller plays well.
- Python controller step: about 0.14 ms; whole loop about 1650 decisions/s (headless, 2 tics per decision).
- 10 host tests pass (`pytest`): features, weight convention, stability bound, left/right turning.

## Phase A status (clock started 2026-10-19 21:15 IST, 1.5 h box)

Findings so far, from live queries against neuPrint:

- Server `neuprint.janelia.org`, dataset **`male-cns:v1.0`** (v0.9 also exists; v1.0 is newer).
- Useful columns: `somaSide`, `consensusNt`, `predictedNt`, `predictedNtConfidence`,
  `superclass`, `synonyms`, `inputRois`, `outputRois`.

| Literature name | Dataset type | Count | Notes |
|---|---|---|---|
| DNa01 | DNa01 | 2 (L, R) | bodyIds 10442 (L), 10760 (R) |
| DNa02 | DNa02 | 2 (L, R) | bodyIds 523769 (L), 10360 (R) |
| MDN | MDN | 4 (2 L, 2 R) | bodyIds 11288, 12348 (L), 10763, 11332 (R) |
| P9 | **not found** | 0 | no type or instance named P9. Regex hits (LoVP9, MeVP9, P6-8P9) are unrelated. The `synonyms` field has no P9 either. |

**P9 deviation:** no neuron is named P9. The only P9-like match in the `synonyms` column is
`DNp36` (`Yu 2010: pIP9`, `Cachero 2010: pIP-h`), bodyIds 10245 (R), 10712 (L), both predicted
glutamate. I use DNp36 as the "forward" output. Whether it is the P9 of the walking literature
is NOT verified, so it is a name-matched stand-in and the forward drive is labelled as such.

### Extraction result (`connectome/extract.py`, run 2026-10-19)

- 300 neurons, 7020 edges (synapse count >= 5 summed over ROIs per pair; total synapses not yet tallied).
- 10 outputs (DNa01 x2, DNa02 x2, MDN x4, DNp36 x2), 14 inputs, 276 hidden.
- Inputs are visual projection neurons reached by the walk (LT51, LC33, LoVC4, MeVPLp1),
  9 left and 5 right, so the "visual superclass" rule worked and no fallback was needed.
- Hop 1: top 100 upstream partners of the outputs by synapse count. Hop 2: top 190 by
  synapses into hop 1. The size cap is enforced by ranking.
- NT counts: 142 acetylcholine, 81 GABA, 75 glutamate, 2 octopamine; none unknown.
- Edge signs: 3408 excitatory, 3299 inhibitory, 313 zero (octopamine neurons, dropped).
- The heaviest edge was cross-checked against `fetch_simple_connections` (totals match).
- A bare `Client(...)` that is not kept in a variable is garbage collected and the default
  client vanishes; the script holds a reference.

## Placeholder graph

`connectome/placeholder.py` writes `connectome/data/placeholder_graph.json`, a random graph
with the real schema, flagged `placeholder: true`. It exists only so Phase B is not blocked.
It is NOT connectome data and nothing built on it may be described as connectome derived.

## Phase C: export to C

- `model/export_c.py` writes `firmware/generated/model_weights.{h,c}` and `tests/golden/rate_golden.csv`.
  The header comment records dataset, version, query date, thresholds, neuron and synapse counts.
- **CSR, not dense.** Dense float32 would be 300 x 300 x 4 = 360 KB; CSR is 6707 non-zeros:
  values 26.8 KB + uint16 column indices 13.4 KB + row pointers 0.6 KB = 40.8 KB (computed).
  **Measured** by cross-compiling for Cortex-M7 (`arm-none-eabi-gcc 10.3.1`, `-mcpu=cortex-m7
  -mfpu=fpv5-d16 -mfloat-abi=hard`, strict warnings, `-Werror`): `model_weights.c` = 40,892 bytes
  read-only data at `-Os` (40,898 at `-O2`); `rate_model.c` = 516 bytes code at `-Os`, 664 at `-O2`.
  These are unlinked object sizes, not a linked image; the real footprint is a Phase D measurement.
  State (r and the scratch buffer) is 2 x 300 x 4 = 2.4 KB. Index types are uint16, guarded by
  `_Static_assert` on neuron count and non-zero count.
- **Why a scratch buffer:** the update is synchronous (every neuron reads the OLD r), matching
  numpy's `W @ r`. Updating in place would change the dynamics.
- **One source of truth:** all model constants (alpha, substeps, tonic drive, input gain, luminance
  weight) are generated into the C header from the Python values, so the two cannot drift apart.
- **Golden test:** the real `PythonController` runs 60 frames of seeded random features and writes
  inputs plus the 4 pooled outputs. The C test replays the same features through
  `rm_make_drive` / `rm_step` / `rm_outputs` and compares with abs 1e-5 + rel 1e-4 tolerance.
  Sums are accumulated in a different order than BLAS, so bit-exact equality is not expected.
- **Checks done:** golden values are non-trivial (0.07 to 0.18); swapping the steer columns in a
  copy of the golden file makes 59 checks fail (so the test can fail); ASan + UBSan clean;
  cppcheck clean; clang-format clean; built with `-Wall -Wextra -Wpedantic -Wconversion -Wshadow
  -Wdouble-promotion -Werror`.
- **A bug the strict build caught:** the generator wrote `5f` for the float 5.0 (invalid C). Fixed in
  `fmt_float`.
- **clang-tidy findings accepted:** `bugprone-easily-swappable-parameters` on the static helper
  `set_drive(drive, idx, count, value)` (internal, 4 call sites, none swapped); in the test file,
  cognitive-complexity warnings caused by the CHECK macro expansion, and `sscanf` for reading the
  golden CSV (host-only test reading a file this repo generated).
- **Not covered by the golden test:** the W[post, pre] convention itself. A transpose in
  `build_weights` would affect Python and C identically. It is pinned by the Python unit test
  `test_weight_convention_post_pre_and_signs` on a 3-neuron graph with a known answer.
- STM32 timing, measured 2026-10-20 with the DWT counter at the 64 MHz default clock: mean 27.27 ms
  (1,745,267 cycles) per model step of 6 substeps over 300 neurons and about 6.7 k non-zero weights,
  i.e. about 43 cycles per non-zero weight per substep. Board and Python activities agree to 3.7e-8. Caches and
  the 400 MHz PLL are the first thing to try for speed; not yet measured.

## MISRA C:2012 deviations (deliberate)

- **Rule 15.5 (advisory), single point of exit:** every function in `rate_model.c` uses early
  returns for its NULL and range guards. This keeps nesting shallow and the guards obvious; the
  alternative would nest the whole body inside the checks.
- **Floating-point equality in tests:** `test_rate_model.c` compares `== 0.0F` for the no-drive case,
  where the exact value is 0 by construction (relu of 0, state starts at 0).
- **Rule 21.6 (standard I/O) in test code:** `stdio` and `sscanf` are used only in the host-only test
  to read the golden CSV. They are never compiled into firmware.
- Not yet checked: a MISRA checker was not run (cppcheck's MISRA addon is not configured); the
  "follows MISRA where practical" claim is limited to the choices above and `-Wconversion`-clean,
  fixed-width, no-malloc, no-recursion code.

## Brain protocol and Phase D firmware (design decisions)

- **Frame: SOF + header + CRC-32, not COBS.** Simple to implement on the ESP32 side and easy to
  read in a logic analyser. Cost: SOF (0xA5) can appear in a payload, so correctness rests on the
  CRC and on resync-by-one-byte (tested, including a false header that swallows a real frame).
  Alternative: reuse secure-boot's COBS + 0x00 delimiter, which resyncs trivially. The CRC is the
  same CRC-32 as secure-boot, so moving to COBS later only changes the framing layer. The version
  byte is in every frame so a v2 fails loudly instead of garbling.
- **Offsets, not packed structs.** Packed struct overlays are a compiler/ABI detail and can cause
  unaligned access on Cortex-M; explicit little-endian put/get helpers are portable.
- **Decode on the board.** The brain returns action bits AND raw activities. The turn rule needs the
  resting outputs, deadzone and sign, which are generated from the Python model into
  `model_weights.h` (one source of truth, no runtime settling). The host cross-checks the board's
  bits against the Python rule (`decode-mismatch` in `play.py`).
- **Q15 features everywhere.** Both backends quantize features to Q15 before the model so
  `--backend python` and `--backend stm32` see identical inputs; otherwise parity would break for a
  reason that has nothing to do with the firmware.
- **Fire is not a brain bit; forward/backward bits are reserved.** Fire is a heuristic, and
  forward/backward have no decode rule or test yet.
- **No HAL, CMSIS only**, same choice as secure-boot (strict -Wconversion/-Werror builds cleanly; HAL
  does not). Vendor CMSIS/startup files are copied into `third_party/cmsis` with their Apache-2.0
  licences, and built without -Werror (the CMSIS include dirs are SYSTEM, so only our code is held
  to the strict flags).
- **Hard-float ABI with the FPU.** secure-boot builds soft-float because it does no float math; a
  float32 model on soft-float would be several times slower.
- **State in DTCM**, weights in flash. The linker asserts at least 8 KB of DTCM stay free for the
  stack. There is no heap.
- **Optional 400 MHz + caches**, as CMake switches (`BRAIN_CPU_400MHZ`, `BRAIN_CACHES`), compiled
  but untested on hardware. 64 MHz HSI is the default so the first flash uses the known-good clock
  (the one secure-boot ran on). The PLL path falls back to HSI if a lock/switch wait times out.
- **Bug class found by the strict build:** the `_init`/`_fini` symbols are missing with
  `-nostartfiles`; empty stubs are in `port/crt_stubs.c`.
- **Parser design:** a fixed buffer, bounded loops (each bad candidate removes at least one byte),
  and a `poll()` to drain buffered frames after a resync. A fuzz test compares the C parser with the
  Python parser on 300 random corrupted streams.
- **Flash address conflict:** secure-boot's bootloader owns 0x08000000 and its app slot starts at
  0x08020400. This firmware is linked at 0x08000000 (as the project spec says), so it replaces the
  bootloader. Delivering it through the bootloader instead needs a link address of 0x08020400 and a
  secure-boot image header; that is the roadmap item.
- **Second UART:** see the README section. Pin facts verified from ST's open pin data; header
  routing and the Ethernet/USB conflict check still need UM2407 (st.com blocked the download).

## ESP32 integration (doom-nano/) design decisions

- **Why a copy inside this repo.** The doom-nano branch of `doom-esp32` was copied here and
  changed. The copy has no history and is GPL; `doom-esp32` was not touched. Alternative: keep it in
  its own repo and send protocol change requests between the two repos. Cost of the copy: two diverging
  versions until the changes are carried back.
- **Protocol source of truth.** The ESP build compiles `firmware/src/brain_protocol.c` and its header
  straight from this repo (a relative path in `brain_link/CMakeLists.txt`) instead of copying them, so
  the wire format cannot drift. If `doom-nano/` moves out into its own repo, that path has to change.
- **The engine never waits for the brain.** One STEP round trip is about 31 ms with the STM32 at
  64 MHz, a Doom tic is 28.6 ms, so a blocking call per tic would halve the game rate. A task on the
  other core steps the brain with the latest features; the engine reads the latest actions each tic.
  Consequence: the brain steps at about 30 Hz of wall time, not once per 2 game tics as in the ViZDoom
  loop (17.5 Hz), so the model's time scale differs from the laptop runs.
- **Features only from fully rendered 3D frames.** The sprite id map and gray view exist only in the
  nano style's frames (menus, intermission, wipes use the outline style). When no such frame has
  arrived for 0.5 s the link task stops stepping and, after 0.25 s without a reply, the engine releases
  every key. No brain, no action.
- **Fire, forward, unstick, respawn are heuristics** because protocol v1 has no outputs for them
  (action bits 2..7 are reserved). The brain's `forward` activity is returned but unused: there is no
  calibrated rest level or threshold for it yet. Alternative for later: add a FORWARD action bit with
  a resting reference decoded on the board, like turning.
- **Kills as score.** The engine's `killcount` restarts every level, so the HUD shows a counter that
  only grows (`ib_session_kills`). It replaced the armor field in the HUD, which the brain cannot
  make use of.
- **Unverified on hardware:** the ESP32 link on the Nucleo (USART1 PB6/PB7, from a Zephyr board file, not yet traced on the board), frame rate with the brain
  attached. Features are not byte-identical to the ViZDoom ones (different luminance source, engine
  sprite map instead of labels), so the tuned constants (input gain, deadzone, turn sign) were not
  re-tuned for Doom and may need adjusting after a real run.


### Making the player move and find monsters (2026-10-20)

- **Player stood still: an input bug, not the brain.** PrBoom drops events while `gametic < 3` and
  clears its key table on every level load, and keys were only posted on a change, so the first
  key-down was lost. Fix: re-post every key once per game tic. Tradeoff: a few extra events per
  tic; the alternative (writing `gamekeydown[]` directly) reaches into engine internals.
- **Timings in game tics, not polls.** `I_StartTic` runs at about 14 Hz on the ESP32 while the game
  runs 35 tics/s, so the stuck detector counted the wrong thing. It now takes the `leveltime`
  delta, so "1 s" means 35 tics whatever the frame rate.
- **The connectome owns turning while a monster is visible.** With no monster on screen its
  outputs stay in the dead zone (luminance alone is too weak), so something else has to explore.
  Heuristics only act when no monster is visible, and each turning tic is logged by source so the
  connectome's share can be reported honestly.
- **Unstick turned left then right and cancelled out.** On E1M2 the player got trapped at a 32-unit
  ledge (Doom's step limit is 24) in a funnel of walls. Now it backs up and turns about 150
  degrees, and a repeat within 3 s keeps the same direction. Turn angles come from PrBoom's
  keyboard turn rates (`angleturn` 320 then 640 after 6 tics).
- **Scan.** Monsters are often behind the player, and the brain only sees the screen. After 8 s
  without one, the player turns a full circle. Alternative considered: a "monster behind" input
  to the brain, which would need protocol v2.
- **Wall avoidance** uses the display pipeline's wall coverage map (centre quarter at least 55 %
  wall). A low ledge only covers part of the height, which is why it missed the E1M2 trap and
  unstick had to handle it.
- **Kill count.** In single player PrBoom's `killcount` includes monsters killed by other
  monsters and barrels (`p_inter.c`). A small hook (`p_player_kills`) counts only the player's
  kills for the HUD; it is the one change inside the engine's game logic.
- **E1M2 at skill 3 instead of E1M1 at skill 2.** Counted from `doom1.wad`: E1M1's nearest monster
  is about 1,700 map units from the start; E1M2 has 11 within 1,000 units.
- **Door probe (E1M3).** E1M3's start room is closed by a door (`DOOR3`, special 31). Wall
  avoidance turned away about 200 units before it, and use reaches 64, so the player circled the
  room. Now a wall ahead triggers, at most every 4 s, 1 s of walking straight at it pressing use.
  Cost: the player bumps into some plain walls (unstick then backs it out). Alternative: ask the
  engine whether a usable line is ahead (more ground truth outside the brain); not done.
- **Auto level exit.** The player cannot navigate to an exit switch, so when `totallive` reaches 0
  the firmware calls `G_ExitLevel()` after 3 s. Labelled as a shortcut, not play.
