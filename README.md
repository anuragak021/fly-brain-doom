# drosophila: a fly-connectome-inspired controller playing Doom

A small neural controller whose wiring is taken from the open Drosophila male CNS connectome
(MaleCNS, neuPrint `male-cns:v1.0`) plays ViZDoom. This is a connectome **inspired** controller,
not a simulation of a fly brain.

## Status

| Phase | State |
|---|---|
| A: extract subgraph (Python) | done |
| B: rate model + ViZDoom loop, Python backend | done |
| C: CSR export to C + host test vs Python | done |
| Brain protocol v1 (header, C + Python implementations, known-answer vectors) | done, host-tested only |
| D: STM32 firmware + `--backend stm32` | flashed to the Nucleo-H753ZI (2026-10-20) and checked with `tools.hw_check`: board matches the Python model, step time measured (below). The `game.play --backend stm32` run and a demo video are still to do |

**Measured on the STM32H753ZI** (`python -m tools.hw_check`, 300 random Q15 feature frames, core at the
default 64 MHz HSI, no PLL, caches off, USART3 on the ST-Link VCP at 115200):

| What | Value |
|---|---|
| board vs Python model, largest activity difference | 3.7e-8 (0 of 1,200 values outside tolerance) |
| turn actions that disagree with the Python rule | 0 of 300 |
| link misses (timeouts / retries) | 0 |
| model step on the board (DWT cycle counter) | mean 27.27 ms (min 27.23, max 27.32), 1,745,267 cycles |
| STEP round trip as seen by the laptop (USB serial included) | mean 31.3 ms, p95 32.8 ms, max 52.8 ms |

The step is slow at 64 MHz: the loop can make about 30 decisions per second. The 400 MHz PLL and
caches build option exists but has not been run on the board. No demo video yet. Everything
labelled "host" below was measured on the laptop or in the host simulator.

## What was built and measured

* Subgraph: **300 neurons, 299,502 synapses** in 7,020 neuron-to-neuron connections
  (each connection has >= 5 synapses; 6,707 have a non-zero signed weight, the rest come from
  octopamine neurons that are dropped). 10 outputs, 14 inputs, 276 hidden.
* Host Python loop (laptop CPU, headless, 2 game tics per decision, features quantized to Q15 as on
  the wire): about 900 to 1,000 decisions/s; controller step about 0.2 to 0.3 ms (an earlier run
  without quantization measured 0.14 ms and 1,650 decisions/s; the laptop timing is noisy). These
  are host numbers, not STM32 numbers.
* Defend-the-center, 15 episodes in 3 runs of 5 (Q15 features): 5 kills in total, 0.33 per
  episode; per-run means 0.4, 0.4 and 0.2 (an earlier unquantized run of 5 averaged 1.0, so the
  run-to-run noise is large). Episodes are short (about 100 to 200 decisions). No baseline was
  run, so this does not show the controller plays well.
* When an enemy was off-centre, the controller never turned away (0 frames in 15 episodes) and
  turned toward it in 55 counted frames (23 of 23 in the first Q15 run, where the total was printed). This
  only shows the pipeline is consistent after the readout sign was chosen to approach; it is not
  independent evidence that the connectome aims.
* C model vs Python reference: 60 golden frames agree within abs 1e-5 + rel 1e-4, including the
  decoded turn actions (`ctest`).
* Brain protocol: 45 Python tests and 2 C test programs pass, including a fuzz test in which the
  C parser and the Python parser produce identical frames on 300 random corrupted streams.
* Weights as CSR in flash: 40,892 bytes of read-only data, measured on a Cortex-M7 cross-compiled
  object (`arm-none-eabi-gcc -Os`, unlinked). The code is 516 bytes. Linked footprint comes in Phase D.

## Simplifications (all of them)

1. Only 300 neurons, chosen by a ranked upstream walk from 10 descending neurons, not the whole brain.
2. Outputs: DNa01 and DNa02 (steering), MDN (backward, logged only), and **DNp36 as the forward
   drive. DNp36 is a name-matched stand-in for P9: no neuron is called P9 in this dataset, and the
   only match is DNp36's synonym `pIP9`. That it is the walking-literature P9 is NOT verified.**
3. Weight = synapse count (>= 5 per pair), row-normalised, scaled to spectral radius 0.9.
4. Sign by presynaptic neurotransmitter: acetylcholine +, GABA and glutamate - (glutamate is
   inhibitory in many but not all fly synapses), octopamine dropped.
5. Rate model (leaky, ReLU, Euler), not spiking.
6. A constant tonic drive (0.2) feeds every non-input neuron. It is not from the connectome.
   Without it the network is silent.
7. Game features: left/right mean luminance and the largest enemy's horizontal position taken
   from ViZDoom's labels buffer. That is game-engine ground truth, not vision.
8. Turn direction is read from the two steering outputs. The connectome's natural response is
   ipsilateral suppression; the readout sign was chosen (`TURN_SIGN = +1`) so the agent turns toward
   the enemy. That choice is mine, not a connectome fact.
9. **Firing is a heuristic**, not connectome driven: fire when an enemy is near screen centre.
10. Forward/backward activities are logged only; the scenario has no move buttons.

## Reproduce

```
uv venv --python 3.14 && . .venv/bin/activate && uv pip install -r requirements.txt   # needs Python >= 3.12
export NEUPRINT_APPLICATION_CREDENTIALS=<your neuPrint token>   # never commit it
python connectome/extract.py            # needs the token; writes connectome/data/*.json
python -m model.export_c                # CSR C files + golden vectors
python -m tools.gen_protocol_vectors    # protocol known-answer vectors
pytest                                  # Python tests
cmake -S tests -B build/tests && cmake --build build/tests && ctest --test-dir build/tests
python -m game.play --backend python --episodes 5

# firmware (needs arm-none-eabi-gcc); 64 MHz default, -DBRAIN_CPU_400MHZ=ON -DBRAIN_CACHES=ON for 400 MHz
cmake -S firmware -B build/fw -DCMAKE_TOOLCHAIN_FILE=$PWD/firmware/cmake/arm-none-eabi.cmake
cmake --build build/fw
tools/flash_brain.sh                    # WARNING: overwrites flash from 0x08000000 (secure-boot's bootloader)
python -m tools.hw_check                # board vs Python parity + DWT step time
python -m game.play --backend stm32     # Doom driven by the board
```

`connectome/placeholder.py` writes a random graph with the same schema, flagged `placeholder`. It
is for development only and is not connectome data.

## Firmware (Phase D)

`firmware/src/main.c` serves the brain protocol on two UARTs at 115,200 baud: USART3 (the ST-Link
virtual COM port, for the laptop) and USART1 (PB7 RX / PB6 TX, Arduino D0 / D1, for the ESP32). Each port has its own
parser (`brain_link`), all share one neuron state, and a reply goes back on the port its request came
from. Only one master should be stepping at a time. Logic lives in host-tested modules (`rate_model`, `brain_protocol`,
`brain_server`); `firmware/port/` holds the thin register-level drivers (clock, GPIO, USART3
with an interrupt-fed ring buffer per port) on CMSIS, with no HAL. Image: 47,068 bytes of flash and
3,104 bytes of RAM (data + bss) (the neuron state is in DTCM), built for the Cortex-M7 FPU with the hard-float ABI.
LEDs: green toggles per STEP, yellow per frame, red on an error or a dropped byte. The firmware is
linked at 0x08000000 as a standalone application, so flashing it **replaces secure-boot's
bootloader**; re-flash that project's image to restore it.

The laptop client `game/stm32_backend.py` implements the same `Controller` interface as the Python
backend. It is tested without hardware against `tests/c/brain_sim.c`, which runs the real parser,
server and model behind pipes: 80 random frames over the wire matched the Python model within 1e-4
and gave identical turn actions.

## Brain protocol (v1)

Owner: this repository. The normative definition is
[`firmware/include/brain_protocol.h`](firmware/include/brain_protocol.h); this section explains it.
doom-esp32 (the ESP32-S3 Doom game) and the laptop `--backend stm32` client are masters of this
link. They must match the header exactly and never redefine it. Reference implementations:
C in `firmware/src/brain_protocol.c`, Python in `game/brain_protocol.py`. Known-answer vectors
(hex frames, CRC check, Q15 examples) are in `tests/golden/protocol_vectors.txt`; both C and Python
tests consume that file, and other projects can read it to check their bytes.

### Frame

```
 off  0     1        2      3     4     5 .. 4+LEN      5+LEN .. 8+LEN
    +-----+--------+------+-----+-----+---------------+------------------+
    | SOF | VERSION| TYPE | SEQ | LEN |    PAYLOAD    |  CRC32 (u32 LE)  |
    |0xA5 |  0x01  |      |     |     |    LEN bytes  |                  |
    +-----+--------+------+-----+-----+---------------+------------------+
                    '----------- CRC covers bytes 1 .. 4+LEN ----------'
```

* All multi-byte integers are **little-endian**. Floats are IEEE-754 binary32, sent as the
  little-endian bytes of the 32-bit pattern.
* `LEN` counts the **payload only** (0 to 32). Frame size is `9 + LEN` bytes, at most 41.
* `SEQ` is chosen by the master and echoed by the brain. `VERSION` is in every frame (currently 1).
* **CRC**: standard CRC-32 (zlib / IEEE 802.3): reflected polynomial `0xEDB88320`, init
  `0xFFFFFFFF`, input and output reflected, final XOR `0xFFFFFFFF`. Check value
  `CRC-32("123456789") = 0xCBF43926`. This is the same CRC as secure-boot's `crc32.c`, Python's
  `zlib.crc32` and the ESP32 ROM function `esp_rom_crc32_le(0, buf, len)`. It is sent after the
  payload, little-endian, and covers `VERSION, TYPE, SEQ, LEN, PAYLOAD` (everything except `SOF`).
* Do not overlay structs on wire bytes; use the offset macros in the header.

### Messages

| TYPE | Name | Direction | Payload |
|---|---|---|---|
| `0x01` | PING | master to brain | empty |
| `0x81` | INFO | brain to master | 9 bytes: `u8` protocol version, `u8` feature count (4), `u16` neurons, `u8` substeps per STEP, `u32` core clock in kHz |
| `0x02` | STEP | master to brain | 8 bytes: 4 x `i16` Q15 features |
| `0x82` | STEP response | brain to master | 22 bytes: `u8` actions, `u8` reserved (0), `u32` step cycles, 4 x `f32` activities |
| `0x03` | RESET | master to brain | empty |
| `0x83` | RESET response | brain to master | empty |
| `0xFF` | ERROR | brain to master | 2 bytes: `u8` code, `u8` offending request TYPE |

STEP response layout (byte offsets in the payload): `0` actions, `1` reserved, `2..5` step cycles
(core clock cycles spent in the model step, DWT; the clock is reported in INFO), `6..9` steer left,
`10..13` steer right, `14..17` forward, `18..21` backward (mean activity of each output group).

Error codes: `1` unknown type, `2` bad length, `3` bad version, `4` bad feature (negative Q15).

### Features (the part most likely to be gotten silently wrong)

Four features, each a real number in 0..1, in this order: `LUM_LEFT, LUM_RIGHT, ENEMY_LEFT,
ENEMY_RIGHT`. A wrong but plausible value still steers the controller, so nothing will error out;
match these definitions carefully.

* **Q15 encoding:** `q = floor(f * 32767 + 0.5)`, clamped by the sender to 0..32767. The real value
  is `q / 32767`. The receiver rejects a negative `q` with error 4. Two implementations may differ
  by 1 LSB; that is acceptable.
* **Luminance:** the mean of an 8-bit grey frame divided by 255, for the left half
  (columns `[0, W/2)`) and right half (columns `[W - W/2, W)`), measured **before dithering** to
  1 bit. The reference is ViZDoom `GRAY8`; palette-to-grey conversion on the ESP32 is an
  approximation. The model was only tuned on ViZDoom frames.
* **Enemy:** pick one visible enemy, the one with the **largest on-screen bounding box**
  (approximately the nearest). Let `x` be the horizontal screen position of its centre, from -1
  (left edge) to +1 (right edge) of the projected image. Then `ENEMY_LEFT = max(-x, 0)` and
  `ENEMY_RIGHT = max(x, 0)`. With no visible enemy both are 0 (an enemy dead centre also gives
  0, 0).
  * A sender that has an angle, not pixels, must use the **projected** position
    `x = tan(theta) / tan(HFOV / 2)`, with `theta` the angle right of view centre. The linear
    `theta / (HFOV / 2)` is wrong.
  * Only enemies the player can **see** count (line of sight; not behind walls). ViZDoom's
    labels buffer never includes hidden enemies, while an engine object list does.
  * In ViZDoom the position is taken from the labels buffer, which is game-engine ground truth and
    not vision. The same simplification applies on the ESP32.

### Actions

Bit `0` = turn left, bit `1` = turn right (mutually exclusive). Bits 2 to 7 are reserved and zero
in v1. **Fire is not a brain bit**: the game side decides it with its own heuristic. Forward and
backward activities are returned as numbers only because there is no decoding rule or test for
them yet.

The brain decodes the turn bits as `diff = ((steer_right - rest_right) - (steer_left - rest_left))
* TURN_SIGN`; turn left if `diff > deadzone`, turn right if `diff < -deadzone`. The resting
outputs, deadzone and `TURN_SIGN` are generated from the Python model into `model_weights.h`, so
there is one source of truth and no settling at runtime. Raw activities are returned too, so a
host can check the STM32's decode against the Python one.

### Link behaviour

* One **master** at a time (ESP32 or laptop), one request outstanding. The brain never sends
  unsolicited frames. The response echoes `SEQ`.
* The master should time out (at least 100 ms for STEP: the model step alone takes 27 ms at the default 64 MHz clock, measured), count a miss, and ignore a late response
  with a stale `SEQ`.
* A frame with a **bad CRC** or `LEN > 32` is dropped silently, with no reply, because its `SEQ`
  cannot be trusted. A frame with a **valid CRC but invalid content** (unknown type, wrong `LEN`
  for its type, wrong `VERSION`, negative feature) gets an ERROR reply that echoes `SEQ`.
* Resynchronisation: after a bad frame the parser moves **one byte past the false `SOF`** and
  rescans (it does not skip `LEN` bytes). `SOF` can appear inside a payload; the CRC settles it.
* A receiver resets its parser if no byte arrives for 20 ms (`BRAIN_INTERBYTE_TIMEOUT_MS`).
* RESET zeroes the neuron state. The resting reference used for decoding is a model constant and
  is not changed by RESET.
* Only **one master may drive the model at a time**. The brain does not arbitrate between the two
  UARTs; if both are active their STEPs would interleave on the same state.

### Computed wire time (not measured)

A STEP request is 17 bytes and its response 31 bytes: 48 bytes, 480 bits at 8N1, which is
**4.2 ms at 115,200 baud** (0.5 ms at 921,600). This is arithmetic, not a measurement; it excludes
the model step and driver latency. The default for the first bring-up is 115,200 baud.

### Second UART for the ESP32 link (USART1 on PB6/PB7, implemented; wiring and board test pending)

USART3 (PD8/PD9) is the ST-Link virtual COM port to the laptop and stays that. The ESP32 link is
**USART1 on PB7 (RX) and PB6 (TX), AF7**, 115,200 baud, APB2 kernel clock. The firmware serves both
ports (flashed and checked on USART3 on 2026-10-20: the laptop path still passes). **USART1 itself
has not been exercised on the board yet**: that needs the wires below and the ESP32.

| Fact | Status |
|---|---|
| PB6 can be USART1_TX and PB7 USART1_RX | **verified** from ST's `STM32_open_pin_data` for STM32H753ZITx (2026-10-20); AF7 from the datasheet's alternate function table |
| PB7 is Arduino D0 and PB6 is Arduino D1 on this board | from Zephyr's `nucleo_h753zi/arduino_r3_connector.dtsi` (fetched 2026-10-20: D0 = PB7, D1 = PB6, D2 = PG14); **not yet confirmed on the physical board**. An earlier version of this README said PG9/PG14 = D0/D1: that was wrong for this board |
| USART6 on PG14/PG9 | still available with `-DBRAIN_ESP_USART6=ON`; PG14 is D2 here and PG9 is not on the Arduino D0 to D15 pins |
| No conflict with Ethernet RMII, USB, LEDs (PB0, PE1, PB14) or button PC13 | LEDs and button from secure-boot's notes; Ethernet and USB pin use **unverified** |

Wiring (3.3 V logic both sides, common ground):

| ESP32-S3 | Nucleo-H753ZI |
|---|---|
| GPIO17 (UART1 TX) | PB7 = D0 (RX) |
| GPIO18 (UART1 RX) | PB6 = D1 (TX) |
| GND | GND |

## Doom on the ESP32-S3 playing with the brain (`doom-nano/`)

`doom-nano/` is a copy of the `doom-nano` branch of the sibling doom-esp32 project (PrBoom on the
ESP32-S3, 128x64 OLED, the "nano" look), with the brain link added. It is **GPL-2.0-or-later** (see
`doom-nano/COPYING` and `THIRD_PARTY.md`); the rest of this repository is separate from it. What
was added here (`doom-nano/components/brain_link`, `components/doom_platform/i_brain_input.c`):

* **Features** (`bl_features.c`): the same four numbers as `game/features.py` (left and right
  luminance, enemy left and right), tested against vectors that Python generates
  (`tools/gen_bl_vectors.py`). Differences from the ViZDoom version: luminance comes from the
  128x56 gray view shown on the OLED (after the display pipeline's black and white levels); the
  enemy is the largest live monster in the engine's sprite id map (engine ground truth, not
  vision).
* **Link task** (`bl_client.c`, UART1 on core 1): sends STEP continuously with the latest features
  and publishes the latest reply. The engine never waits for the brain.
* **Controls** (`bl_policy.c`, `i_brain_input.c`): **turning is the connectome's** (the board's action
  bits become left/right arrow key events). These are **heuristics, not the connectome**: fire when
  a monster is within 0.15 of the screen centre; walk forward whenever not aiming; if forward
  walking makes no progress for 1 s, turn right and press use for 0.5 s (doors, corners); press use
  to respawn after death and to dismiss intermission screens. Turn decoding uses the board's
  action bits; nothing is decoded again on the ESP32.
* **Score**: the OLED's bottom line shows health, **kills** (skull icon, summed over lives and
  levels) and ammo. The armor field was replaced. Kills and deaths are also logged on the console.
* The game starts E1M1 at skill 2 directly (`-warp 1 1 -skill 2`, menuconfig `doom-esp32 brain
  link`); without the brain the player stands still.

Status: **builds** with ESP-IDF 5.5.2 (`-Werror`), and 12 host test suites pass (feature vectors,
policy, HUD). **Not yet run on the ESP32 with the brain**: the board was not attached to this
computer when this was written, so there are no kills, frame rate or demo numbers for the
combined system yet.

## Roadmap

STM32 brain firmware with a framed UART protocol and DWT step timing (Phase D); Doom on an ESP32-S3
with OLED; COBS/CRC reuse from the secure-boot project; GPU simulation of a larger network;
fixed point + CMSIS-DSP; spiking variant; delivery through the secure bootloader. See `NOTES.md`.
