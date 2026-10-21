# NOTES: design decisions for doom-esp32

Plain language log of what was decided, why, and what the alternatives were. Newest phase at the bottom.

## Phase 0: environment and board identification (2026-10-19)

### Toolchain
* ESP-IDF v5.5.2 at `~/esp/esp-idf` (activate with `. ~/esp/esp-idf/export.sh`). The Xtensa toolchain
  covers both esp32 and esp32s3.
* Host tools: gcc, cmake, cppcheck, clang-format, clang-tidy, Python 3.10.

### Serial port map on the laptop (checked by USB vendor/product ID, not guessed)
| Port | USB chip | Board |
|---|---|---|
| /dev/ttyACM0, ttyACM1 | QinHeng CH342 dual serial (1a86:55d2) | FRDM i.MX 93 debug UART |
| /dev/ttyACM2 | STLINK-V3 (0483:374e) | Nucleo H753ZI. Never flash from this project |
| /dev/ttyACM3 | QinHeng CH343 single serial (1a86:55d3), serial 5B14083969 | ESP32-S3 (this project) |

ttyACM numbers depend on plug order. Before every flash, re-check by USB serial number
(`udevadm info -q property -n /dev/ttyACMx | grep ID_SERIAL_SHORT`), not by number.

### The "home ESP32" is actually an ESP32-S3 with PSRAM
`esptool.py flash_id` (read only) reported:
* ESP32-S3 (QFN56) revision v0.2, MAC 9c:13:9e:ac:3a:2c, 40 MHz crystal
* Embedded PSRAM 8 MB (AP_3v3), which is the octal PSRAM inside the chip package (ESP32-S3R8)
* Flash: 16 MB, manufacturer 0x68, device 0x4018, quad mode, 3.3 V

So this board is in the same class as the N16R8 target (16 MB flash, 8 MB octal PSRAM). The
board previously ran the lyrics player.

### Backup of the lyrics player firmware
The full 16 MB flash was read to `~/backups/lyrics-player-esp32s3-9c139eac3a2c-2026-10-19.bin`
before any write: 16777216 bytes, sha256
`eeddf744e5deaa394eff7e92078ce878900f4213dc976f1b620e16519b2ee032`. Restore with
`esptool.py --port <port> write_flash 0 <file>`.

### OLED
The lyrics player source says SSD1306 128x64 at 0x3C, SDA GPIO8, SCL GPIO9. Its OLED code is
compiled out (`#if 0`) because the OLED was once suspected dead, so the boot log proves nothing.
Address 0x3C alone does not tell SSD1306 from SH1106 (both use it). The SH1106 has 132 columns
shown with a 2 column offset, and it ignores the SSD1306 horizontal addressing command (0x20).
A test pattern with lines at column 0 and 127 tells them apart.

`tools/oled_probe` was flashed on 2026-10-19 (after confirming the serial port). Results:
* PSRAM works: AP octal, 64 Mbit (8 MB), generation 3, 80 MHz, boot memory test OK, 8192 KB
  added to the heap.
* I2C: **no device answered** on SDA/SCL pairs 8/9, 4/5, 17/18, 1/2, 41/42 (internal pull-ups
  on, 400 kHz). The cause turned out to be simple: the OLED was not connected.
* After wiring (VCC 3V3, GND, SDA GPIO8, SCL GPIO9), a device answers at **0x3C on SDA=8/SCL=9**.
  Reading one byte returns status 0x05, which does not settle SSD1306 vs SH1106 on its own.
  The test pattern was sent with ESP_OK; the controller type comes from what the screen shows.
* **Result: SSD1306, 128x64, I2C at 0x3C.** The screen showed the full rectangle with lines on both
  the left and right edges and the block at the upper left. An SH1106 would hide column 0 and
  show noise at the right edge. The module is a 0.96 inch board with a 4 pin header
  (GND VDD SCK SDA, where "SCK" is the I2C clock). It is not SPI: SPI modules have 6 or 7 pins
  (CS, DC, RES), and this one acknowledged an I2C address. The orientation commands A1/C8 give
  a correctly oriented image.

Final pin map for the home board: OLED SDA GPIO8, SCL GPIO9, VCC 3V3, GND. GPIO8/9 are not
strapping pins, not flash/PSRAM pins (26 to 37) and not USB (19/20).
* The USB cable is now on the S3's native USB port: /dev/ttyACM3 is now Espressif 303a:1001,
  serial = MAC 9C:13:9E:AC:3A:2C (the same chip esptool identified). Logs still appear because
  IDF mirrors the console to USB Serial/JTAG. A reset over this port re-enumerates USB, so
  readers must reopen the port.

### Repo hygiene
* `sdkconfig` is gitignored. `sdkconfig.defaults` is the source of truth, which keeps diffs
  reviewable and avoids committing machine specific generated config.
* `*.wad` is gitignored. WAD files are never committed.

## Phase 1: display pipeline (components/oled_pipeline)

Pure C11, no IDF calls, so the same files build for the ESP32-S3 and for host tests.
Four stages, one module each (`op_gray`, `op_scale`, `op_dither`, `op_pack`), all returning
`op_status_t` and validating every size before touching memory. No heap. Buffers are either
passed in by the caller or are small fixed stack arrays (at most 128 entries) whose size is
checked against `OP_MAX_DST_WIDTH_PX`.

### Stage 1: palette to gray LUT
* What: Rec.601 luma `(77 R + 150 G + 29 B) / 256` per palette entry, followed by an optional
  gamma (`gamma_x100`, where 100 means unchanged) folded into the same 256 entry table.
* Why a LUT: Doom pixels are palette indices, so a lookup costs 1 load per pixel, and all the
  math (including powf) runs only when the palette changes.
* Why built at runtime and not a const table: PrBoom switches palettes for damage (red) and
  pickup (gold) flashes through `I_SetPalette`. A runtime LUT follows those flashes for free.
* Levels (black and white points) plus gamma, all fixed and folded into the LUT. Measured on 120
  downscaled ViZDoom frames (every 5th of 300 Freedoom E1M1 frames and 300 deathmatch frames),
  luma percentiles 2/10/50/90/98 are 6/23/39/59/85 (E1M1) and 5/29/57/103/120 (deathmatch).
  Doom is dark: almost nothing is above 128. With the raw luma, Bayer lights only 2 to 4 of 16
  pixels in most areas and the frame reads as sparse noise. Default levels black=8, white=128
  stretch that band to the full range. Preview images (docs/preview) show bricks, doorways,
  corridor perspective and the weapon clearly afterwards. Gamma stays at 1.0 by default
  because levels alone fixed the problem.
* Why not auto levels: per frame auto levels would make brightness pump when the view moves
  from a dark room to a lit one, and on a 1 bit display that looks like the whole screen
  flickering. Fixed levels are stable. Tradeoff: very dark sectors clip to black, and bright
  areas (sky, light sources) clip to white.
* Bug caught while tuning: a numpy prototype of the levels LUT overflowed (`150 * uint8` stays
  uint8 under NumPy 2 promotion rules) and made the image darker. The C version accumulates in
  uint32, and the prototype now first checks that it matches the C LUT exactly at identity.
* Alternative: Rec.709 weights. The difference is invisible after 1 bit dithering, and Rec.601
  matches Doom's era.

### Stage 2: crop and area-average downscale
* Crop vs letterbox: the 128x64 panel is 2:1. Doom's 320x200 frame is shown at 4:3, so a
  pixel is 1.2 times taller than wide. A 2:1 window with correct proportions is
  320 x 320/2/1.2 = 320 x 133 source rows. We **crop** that window from the 168 row 3D view
  above the 32 row status bar: 17 rows of ceiling and 18 rows of floor/weapon are lost.
  * Letterboxing the whole 320x168 view would need 128x56 with bars, so 12% of an already tiny
    screen would be wasted. Squashing 168 rows into 64 would distort proportions by about 17%.
  * The status bar is dropped because at 64 px its digits are unreadable. Health and ammo can
    come back later as a text overlay.
  * The crop is a struct (`op_scale_cfg_t`), not hardcoded, so other choices are one line away.
* Area averaging, not nearest neighbour: each output pixel is the exact mean of the 2.5 x 2.08
  source area it covers, including fractional edge pixels. Nearest neighbour drops 84% of the
  pixels, which aliases thin lines (door edges, distant enemies) into flicker as the view moves.
* How (integer only): source column c covers `[c*dst_w, (c+1)*dst_w)` and output column x
  covers `[x*crop_w, (x+1)*crop_w)` on a common grid. The overlap is an integer weight. Weights
  sum to `crop_w * crop_h`, so one integer divide per output pixel gives the exact rounded mean.
  The accumulator is at most 255 * 320 * 133 = 10.9 M, which fits in uint32 (checked in code).
* The gray LUT is applied inside the scaler, so no 64000 byte gray copy of the frame is needed.

### Stage 3: dithering to 1 bit
* Bayer 4x4 (ordered): a pixel is on when gray > `16 * B[y%4][x%4] + 8`. That gives 17 exact
  levels per 4x4 tile. Output depends only on (x, y, gray), so a pixel whose value does not
  change between frames never flips. A test proves that changing one input pixel changes at
  most one output pixel.
* Floyd-Steinberg: error diffusion 7/3/5/1 in sixteenths, serpentine scan to avoid the diagonal
  "worm" pattern. The integer split gives the remainder to the right neighbour, so no error is
  lost to rounding (a test checks that the mean is preserved). Two int16 error rows live on the
  stack with guard cells, so there are no bounds checks in the inner loop.
* Default = Bayer. For moving video, error diffusion is chaotic: a 1 level change anywhere can
  re-route error through the rest of the frame, so static walls "boil". Floyd-Steinberg shows
  more detail in a single still. Both are kept and selectable with `op_dither_mode_t`.
* Measured (host/preview/preview.py --stats, levels 8..128, real C code via ctypes):

  | % of OLED pixels that flip | Bayer 4x4 | Floyd-Steinberg |
  |---|---|---|
  | per frame, 300 frame E1M1 walk (2 tics/frame) | **7.61** | 37.45 |
  | per frame, 300 frame deathmatch walk | **10.16** | 39.11 |
  | still frame, every gray level +1 | **0.16 to 0.55** | 34.07 to 36.17 |

  The last row is the key one. A one level brightness change, which is invisible in gray, makes
  a third of the Floyd-Steinberg screen change; Bayer barely moves. During motion, Floyd-
  Steinberg flips about 4 times as many pixels as Bayer. Bayer stays the default. CPU cost per
  frame on the ESP32-S3 is measured in the hardware demo below.

### Stage 4: SSD1306 page packing
* Byte `page*128 + x` holds column x of rows `page*8 .. page*8+7`, with bit 0 at the top, which
  is the SSD1306 horizontal addressing layout. A whole frame is 1024 bytes and one I2C write.
* Kept separate from dithering so the bit layout is tested on its own and could be swapped for
  another controller (for example an SH1106 page-mode writer).

### Testing
* `host/` CMake project with a small assert runner (`host/tests/test_runner.h`): CHECK keeps
  going after a failure so one run lists every broken expectation. ctest runs 4 suites, built
  with `-Werror -Wconversion -Wdouble-promotion` and ASan + UBSan.
* The same sources are also built as `liboled_pipeline.so` so that the Python preview and
  stream tools call the exact C code that runs on the ESP32 (through ctypes) and do not
  reimplement it.
* cppcheck (warning, style, performance, portability) is clean, rechecked after Phase 2 on all
  pure C modules (oled_pipeline including op_outline, demo_stream, wad_check). clang-tidy (bugprone, cert,
  clang-analyzer, readability) is clean, with these deliberate exclusions: magic-numbers
  (constants are named; the tests use literal expected values), identifier-length (x, y, i are
  clearer in pixel loops), and uppercase-literal-suffix (`0.5f` is the common C style).

## Phase 1 on hardware: first frames, and why the picture changed

### Link bring-up (2026-10-19)
* The demo first used the S3's native USB Serial/JTAG. Under WSL2 that device is forwarded by
  usbipd and drops off whenever the chip resets (it re-enumerates). The user then moved the
  cable to the COM connector (CH343 bridge, 1a86:55d3, serial 5B14083969) and the demo
  transport became UART0 (Kconfig `DEMO_TRANSPORT`, both backends kept).
* First symptom: no "hello" from the board at 2 Mbaud. A passive read showed that opening the
  port resets the chip (auto-reset circuit, `rst:0x1`) and that it boots normally from flash,
  but no packets arrived. Two changes were made in the same test build: UART0 pins set
  explicitly (`uart_set_pin` to GPIO43/44, since with the console disabled nothing else
  guarantees the routing) **and** the baud dropped from 2 Mbaud to 115200. Frames then reached
  the OLED (seen on the real panel). Which change fixed it is **not yet known**: the 2 Mbaud
  build with the pin fix has not been run. Open until it is.
* An earlier "multiple access on port" error came from `idf.py monitor` and the streamer having
  the same tty open; the streamer now says so instead of printing a traceback.
* The OLED keeps its RAM across ESP32 resets (and holds noise after power up), so the demo now
  draws a boot screen (blank with a centre bar). Without it a running demo looks dead.

### Outline style (new default, replaces plain Bayer)
* What I saw on the real panel with plain Bayer: a uniform dot texture where the weapon
  and enemies could not be found. Two causes:
  1. The centred crop cut the bottom 18 rows of the 3D view, which is where the weapon sprite
     sits. **Fix: bottom aligned crop** (`crop_y = 35`), dropping ceiling instead, which carries
     the least gameplay information.
  2. After levels, walls are mid gray and Bayer renders every texture as the same dot field,
     so nothing has a silhouette.
* Six variants were compared side by side on 4 captured frames (`host/preview` style sheets):
  centre crop, bottom crop, darker levels, unsharp mask, half fill + raw Sobel edges, and edges
  only. Then 4 tunings of the edge variant. Winner, now `op_outline()`:
  Bayer fill at gray/2, plus every pixel forced on where the Sobel magnitude |gx|+|gy| of a 3x3
  box blurred frame is > 90. The blur suppresses brick-mortar texture so mostly geometry and
  sprite silhouettes are outlined. The weapon (with muzzle flash) and an enemy are clearly
  visible in docs/preview/freedoom1_f200.png; plain Bayer shows them as faint blobs.
* Levels default changed from 8..128 to 16..170: with outlines carrying the shapes, a darker
  fill reads better (less of the screen is lit noise).
* Flicker measured as before (levels 16..170):

  | % pixels flipping | outline | Bayer 4x4 | Floyd-Steinberg |
  |---|---|---|---|
  | per frame, E1M1 walk | 15.89 | 5.77 | 26.59 |
  | per frame, deathmatch walk | 17.66 | 7.39 | 35.71 |
  | still frame, every gray level +1 | 0.09 | 0.17 to 0.29 | 27.04 to 36.33 |

  Outline flickers more than Bayer during motion because edges move with the scene. That is
  real motion, not noise: on a still frame it is the most stable of the three.
* Tradeoffs: about 2x the CPU of Bayer (blur + Sobel per pixel; to be measured on the S3), an
  8 KB static scratch buffer (not reentrant, documented in the header), and busy surfaces
  (brick walls seen at a grazing angle) still produce many edges.
* Alternatives not taken: per-frame auto contrast (pumps), unsharp mask (amplified texture
  noise, see sheets), edges only (lost floors and walls, so there was no depth cue).

## Brain protocol header landed (read only, owned by drosophila)
`drosophila/firmware/include/brain_protocol.h` (v1) now exists, so no local draft is needed.
Points that constrain this project, recorded before Phase 3:
* Frame: SOF 0xA5, VERSION, TYPE, SEQ, LEN, payload, CRC-32 (zlib) over VERSION..payload,
  little endian. Our demo stream uses the same CRC (`ds_crc32`), but the two protocols are
  separate and must stay that way.
* STEP features are Q15 in 0..1. **LUM_LEFT / LUM_RIGHT are the mean of the un-stretched 8
  bit gray before dithering**, over the left and right halves. The display LUT applies levels
  (16..170) and the crop drops the ceiling, so the feature extractor must use a plain luma
  LUT over the full rendered view, not the display path. Using the display gray would bias the
  brain's input.
* ENEMY_LEFT / ENEMY_RIGHT use the projected position `tan(theta) / tan(HFOV / 2)` of the
  largest visible enemy, with line of sight checked, not the linear angle.
* The master (this ESP32) sends one request and waits; suggested STEP timeout 20 ms, then
  count a miss. FIRE is decided on the game side, not by the brain.

## Phase 2: PrBoom on the ESP32-S3 (boots on the board)

### Choosing the port
Searched GitHub first (`gh api search/repositories`). Candidates:
* **ducalex/retro-go `prboom-go`** (chosen): PrBoom 2.5.0, GPL-2.0, maintained (last commit
  2026-01-19), has an `esp32-s3-devkit` target. Its engine directory is self contained; every
  retro-go dependency in it is behind `#ifdef RETRO_GO` (checked by grepping the whole tree),
  so leaving that undefined decouples it with no shim. Its `w_wad.c` already supports a WAD
  held in memory (`wadfile->data`), returning lump pointers into it without copying, which is
  exactly what an mmapped flash partition needs.
* espressif/esp32-doom (Sprite_tm): the original proof of concept, but from the IDF 3 era, last
  pushed 2023, and written for the plain ESP32.
* cocus/esp32-ili9341-doom-lvgl and other small S3 ports: tied to one LCD and board, with little
  history.

Imported `prboom-go/components/prboom` byte-exact from commit
`4ced120669750ca7228fd0414211430c1d923166` (`.gitattributes` marks it `-text` so the upstream
CRLF files stay unchanged; `diff -rq` against the clone is clean). The upstream `data/` folder
(doom1.wad, prboom.wad) was left out: no WADs in git. `prboom_wad.h` (prboom.wad compiled in,
GPL) is kept.

### What is ours and what is theirs
* Theirs, unmodified except three marked patches: `components/prboom/` (engine).
* Build glue: `components/prboom/CMakeLists.txt` rewritten (the upstream one used the legacy
  `register_component()` and retro-go's `rg_setup_compile_options()`). Same flags as upstream,
  plus `-DDOOM_ESP32` and `-Wno-error`, because IDF adds `-Werror=all` to every component and
  third party code is not reformatted or fixed here. Result: one engine warning
  (`p_user.c:314`, `-Waddress`, a harmless always-true pointer test).
* License: engine files are "GPL version 2 or (at your option) any later version", so the
  firmware as a whole is GPL-2.0-or-later. `COPYING` (GPL v2 text) is at the repo root.
* Ours: `components/doom_platform/` (the I_* interface), `main/`, `partitions.csv`,
  `sdkconfig.defaults`, `tools/flash_wad.py`. Zero warnings with our `-Werror` flag set.

### Engine patches (each marked `/* doom-esp32 patch: ... */`)
1. `d_main.c`, `D_AddFile`: under `#ifdef DOOM_ESP32`, ask the platform `I_MapWadFile()` for
   the file first. On success, register the mapped bytes the same way the engine registers its
   bundled prboom.wad, and skip `access()`. `CheckIWAD` and `W_CacheLumpNum` already handle
   `data`, so nothing else changed.
2. `r_things.c`, `R_DrawMasked`: under `#ifdef DOOM_ESP32`, call `I_SpritesBegin()` after the
   vissprites are sorted and `I_SpritesEnd()` after the player sprites. No engine logic changes.
   Layers: the platform only reads the framebuffer at those two points (see "Layers style"
   below). Nano: it clears and finishes the sprite id map there (see "Nano sprites").
3. `r_main.c`, `R_RenderPlayerView`: under `#ifdef DOOM_ESP32`, call `I_WallsBegin()` just
   before `R_RenderBSPNode` and `I_WallsEnd()` right after the following
   `R_ResetColumnBuffer()` (which flushes the buffered wall columns). The platform fills the
   view with a marker colour at the first point and reads the framebuffer at the second (see
   "Nano style" below). Only on branch `doom-nano`.
4. `r_things.c`, `R_ProjectSprite`: set `vis->thing = thing`. The `vissprite_t` field exists
   upstream but is never filled. Only on branch `doom-nano` (patches 4 to 7: "Nano sprites").
5. `r_things.c`, `R_DrawVisSprite`: call `I_SpriteThing(thing or NULL for the weapon,
   is_weapon, is_fuzz)` before the column loop and `I_SpriteDone()` after it.
6. `r_things.c`, `R_DrawMaskedColumn`: call `I_SpritePost(x, yl, yh)` just before `colfunc`,
   inside the existing clip check. This function also draws masked mid textures (from
   `r_segs.c`); those posts arrive between `I_SpriteDone` and the next `I_SpriteThing`.
7. `p_inter.c`, `P_DamageMobj`: call `I_ThingDamaged(target)` right before the damage is
   subtracted (after the early returns for things that cannot be hurt and god mode).

### Platform layer decisions
* **WAD from flash, in place.** The `wad` partition (6 MB, subtype 0x40, offset 0x210000, 64 KB
  aligned) is mapped once with `esp_partition_mmap` and never unmapped. Lumps are read through
  the flash cache: no 4 MB copy in RAM. Before the engine sees it, `wad_check()` (pure C, 7 host
  test cases) validates the magic, the lump count, the directory and every lump against the
  mapped size. Erased flash (0xFF), a partial write and wrapped offsets are all rejected. On
  the real DOOM1.WAD it reports 1264 lumps, ending exactly at the 4,196,020 byte file size.
* **Only the IWAD name is mapped** (`doom1.wad`, passed as `-iwad`), so the engine's search for
  other IWAD names cannot pick up the same bytes under a different name (the name matters for
  TNT/Plutonia detection).
* **No filesystem.** newlib on ESP-IDF declares `access()`, but no VFS provides it, and the
  engine needs it. Ours always reports ENOENT. Config, savegame and demo file opens fail, and
  the engine falls back to its defaults (checked in `M_LoadDefaults`).
* **Video:** the engine draws 320x200 indices into a static 64000 byte framebuffer in internal
  RAM (DRAM_ATTR). `I_FinishUpdate` runs the Phase 1 pipeline: the LUT built from the **raw
  PLAYPAL** bytes (not `V_BuildPalette`, which returns RGB565 with gamma applied), the bottom
  aligned crop and area downscale, the display style chosen in menuconfig
  (`DOOM_OLED_STYLE`: nano, the default on branch `doom-nano`; layers; outline), with outline
  for frames that have no engine masks, packing and the I2C write. It times each
  stage and logs averages every 100 frames.
* **Memory:** `heap_caps_malloc_extmem_enable(0)`, so every engine malloc (zone, caches) goes to
  the 8 MB PSRAM, as in the reference port. Static internal RAM is 68.9% used (235 KB, which
  includes the framebuffer); it was 66% before the layers style added its 8 KB coverage
  buffer. On branch `doom-nano` it is 71.3% (243,559 bytes): another 8 KB for the wall
  coverage. The sprite snapshot is in PSRAM (see "Layers style"); on the branch it is sized
  for the whole 3D view (53,760 bytes), because the nano crop is a different window. The image is 686 KB; the app partition is 2 MB, leaving room for
  Phase 3.
* **Time:** `I_GetTime` is computed in 64 bit. The reference port's `int` expression
  `ms * 35 * 100` overflows after about 10 minutes of uptime.
* **Sound:** silent stubs and `-nosound`. The engine's globals `snd_card`, `mus_card` and
  `snd_samplerate` (defined in retro-go's main.c) are defined in `i_sound_null.c`.
* **Watchdog:** the engine loop never blocks, so the CPU0 idle task cannot run, and watching
  it would give false alarms. Its check is off (`CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0=n`).
  The engine task is watched instead: it is added with `esp_task_wdt_add(NULL)` and fed in
  every `I_FinishUpdate`. It only warns (the IDF default, no panic), so a slow level load cannot
  cause a reboot loop. The interrupt watchdog alone would not catch a task that spins with
  interrupts enabled. Main task stack 32 KB (the BSP walk recurses). FreeRTOS at 1 kHz
  so `I_uSleep` has millisecond resolution.
* **No input yet:** `I_StartTic` is empty, so the engine runs attract mode (title screen and the
  WAD's demos). That is enough for the first boot and for a frame timing measurement.

### First boot (2026-10-20)
* Flashed by hand. The first attempt showed the old stream demo's waiting splash and its
  `OLS1` hello on the console: the WAD had been written, but the Doom app had not (the board
  still held demo/oled_stream). After the Doom app was flashed, the attract demos ran
  on the OLED (user photos). No crash was reported. FPS and the stage timings are in the
  monitor log and have not been recorded here yet.
* The photos showed the outline style is too busy on DOOM1.WAD's real art: dot fill
  everywhere, edge lines on every wall texture, and the weapon and monsters lost in it. That
  led to the layers style below.

### Layers style (default for rendered 3D frames)
* **Getting test frames that match the board.** The first host captures (E1M1, random walk)
  were mostly wall close-ups and did not look like the photos. The shareware attract demos
  play **E1M5, E1M3 and E1M7** (read from the DEMO1..3 lump headers: version 109, skill 2), so
  `capture_frames.py` gained `--iwad` and `--map`, and the comparisons use those maps with
  DOOM1.WAD's own palette.
* **Image-only candidates** (no engine help), compared side by side on those maps:
  A) outline with fill/4 and threshold 150, B) gamma 1.5 then Atkinson, C) unsharp mask then
  Bayer, D) unsharp mask then Atkinson, E) gamma 2 then Atkinson plus edges at 160. B, D and E
  are cleaner on stills, but Atkinson is error diffusion: E flickered on 20 to 30% of pixels
  per frame on E1M3/E1M7 (outline: 11 and 6.5%). None of them separates a monster from a wall
  of the same brightness, which is what I wanted ("I should know where the gun and
  everything is").
* **Idea: use what the engine knows.** Doom draws the level first, then all sprites
  (monsters, items, projectiles) and last the player's weapon, all inside `R_DrawMasked`. The
  pixels that change between the start and the end of that function are exactly the sprite
  pixels. Two hooks (engine patch 2) take a copy of the crop window (320x133 = 42,560 bytes) at
  the start and compare at the end (`op_mask_diff`), and the 0/1 map is area-averaged to the
  OLED grid with the same crop and weights as the gray frame (`op_mask_coverage`, which reuses
  `op_scale_area` with a LUT mapping 0/1 to 0/255; a host test checks that the two grids line
  up exactly).
* **The style** (`op_layers`): a pixel with sprite coverage >= 50% is "sprite". Level pixels
  get a Bayer fill at gray/4 plus Sobel edges of the blurred frame above 160 (only strong
  geometry: wall corners, floor/wall lines). Sprite pixels get a Bayer fill at gray*19/16 + 70
  (bright, so dark monsters still show), and every level pixel touching a sprite is forced off:
  a one pixel black halo that separates the sprite from the wall even when they have the
  same gray. All Bayer (position based), so no error diffusion shimmer.
* **Measured on host** (C library through ctypes, DOOM1.WAD, 150 frame walks per map):

  | % pixels flipping per frame | layers | outline | gamma 2 + Atkinson + edges |
  |---|---|---|---|
  | E1M5 | 7.6 | 14.7 | 8.4 |
  | E1M3 | 8.0 | 11.0 | 30.3 |
  | E1M7 | 4.0 | 6.5 | 20.0 |

  Lit pixels: layers 11 / 19 / 22% vs outline 21 / 37 / 47%, so the panel is about half as busy.
* **How the host previews get a sprite mask:** ViZDoom's labels buffer, keeping only the values
  listed in `state.labels` (the raw buffer also marks floors and ceilings), plus the weapon
  from a twin run with `set_render_weapon(False)` and the same seed and actions. This is an
  approximation: the twin runs drifted apart on 10 to 27% of frames, and ViZDoom labels some
  effects (bullet puffs) as large blocks. The board's mask is exact by construction. The
  comparison sheets use id's DOOM1.WAD art, so they are not committed; the scripts were
  throwaway.
* **Known limits:**
  - Masked mid textures (grates, see-through bars) are drawn in the same function, so they
    count as sprites and are drawn bright.
  - A sprite pixel with the same palette index as the wall behind it is not detected. Only
    isolated pixels are affected; the area average absorbs them.
  - Menus drawn over a running game are not sprites, so they render as dim level. Frames with
    no 3D view (title, intermission, the frames of a screen wipe) have no mask and fall back
    to the outline style; the mask is cleared after every `I_FinishUpdate`, so it is used for
    at most one shown frame. One known glitch: `D_Display` renders the new view and then runs
    the wipe loop, so the **first** melt frame of a wipe is drawn with the new view's mask
    (one frame, about 1/30 s, at level start). Fixing it needs a third engine hook in the wipe
    code; not worth it yet.
* **Cost:** one 42 KB copy into PSRAM and one 42 KB compare per frame, plus the coverage
  downscale. The snapshot is in PSRAM (`EXT_RAM_BSS_ATTR` with
  `CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY`, verified at 0x3C090000 in the ELF) to keep
  internal RAM free. Not measured yet: the frame log prints it as `sprites`, with
  `styled n/100` showing how many frames used the configured style (on main the names were
  `mask` and `layers n/100`).
* **Alternatives not taken:** patching the column drawers to write a mask (touches the
  engine's hottest code); reading the vissprite list (gives boxes, not pixels); two snapshots
  to treat the weapon differently from monsters (not needed yet; one mask gives both the
  same look).

### Nano style (branch `doom-nano`, default there)
* **Why.** The reference was QC20/Doom-run-on-Arduino-Pro-Mini-and-SSD1306, a copy of
  daveruiz's doom-nano: a small raycaster written for the same 128x64 SSD1306; the goal was
  its look. That repo has no license, so **no code, font or image
  from it is used**; only the look was studied and rebuilt here for real Doom frames. Kept on
  a local branch, so `main` stays the layers build.
* **What makes doom-nano readable** (from its screenshots and from reading its renderer):
  every wall column is one flat shade from a few Bayer levels, the wall's top and bottom pixel
  are lit, floors and ceilings are black, sprites are solid white shapes, and a text HUD sits
  at the bottom. Very few pixels change per frame and nothing has texture noise.
* **The wall mask (engine patch 3).** Flat wall columns need to know which pixels are walls.
  Before the wall pass, `I_WallsBegin` fills the part of the crop that the 3D view covers with
  palette index 251. Walls overwrite it; floors, ceilings, sky and sprites come later. Right
  after the walls, `op_mask_wall_coverage` counts every pixel that is not 251 as wall and area
  averages that to the view grid in one `op_scale_area` pass (LUT: 251 -> 0, everything else
  -> 255). **Why 251 is safe:** I checked DOOM1.WAD v1.9. The 163 patches used by TEXTURE1
  (there is no TEXTURE2) use 213 distinct palette indices, and none of them maps to 251
  through any of the 34 COLORMAP rows. Index 251 only comes out of rows 0..3, and only from
  texel 251 itself, which no wall patch uses. This is a property of that WAD. Recheck for any
  other IWAD; a wrong marker only makes those wall pixels count as "not wall" (black).
* **The style** (`op_nano`, chosen by me from side by side scratch mockups on the attract
  demo maps, variant "N6 + split at 40"; not yet reviewed side by side on the panel):
  - sprite (coverage >= 50%): on if gray > 40, so sprites are crisp and only their darkest
    parts drop out; one pixel black halo around sprites, as in the layers style. (First
    version, commit 186df99. Replaced by per class sprites, see "Nano sprites" below.)
  - wall: each vertical run of wall pixels in a column is cut where the gray steps by more
    than 40 between rows (stacked walls, steps, a lit band). Each run is shaded with its mean
    gray mapped to `clamp((mean - 12) * 2)`, quantized to 5 levels with level 1 as the
    minimum (far walls never vanish), and Bayer dithered. The first pixel of each run and the
    last pixel of each wall are lit, which draws the wall outlines and the step lines;
  - floors, ceilings and sky: off.
* **Layout:** 3D view 128x56 above an 8 row HUD. Same proportions rule as the default crop:
  320 / (128 / 56) / 1.2 = 117 source rows, bottom aligned on the 3D view (rows 51..167), so
  the weapon stays in view. The HUD (`op_hud`): separator line, then health, kills and the
  ready weapon's ammo (hidden for fist/chainsaw), each with a 5x5 icon (cross, shield,
  bullet) and a 3x5 digit font drawn for this project. Values come from
  `players[displayplayer]` and are clamped to 0..999.
* **Fallbacks:** frames without both masks (title, intermission, automap, wipe frames) and
  frames with the menu open use the outline style over the full 128x64 with no HUD. The menu
  check is there because the menu is drawn after the masks were taken, and the nano style
  would black out its text on floors.
* **Measured on host** (C code through ctypes, DOOM1.WAD, 150 frame ViZDoom walks; masks from
  ViZDoom's labels buffer, wall = label 0, so the same approximation as for the layers
  previews):

  | per frame | nano | layers |
  |---|---|---|
  | % pixels flipping, E1M5 / E1M3 / E1M7 | 9.7 / 9.3 / 5.0 | 7.6 / 8.0 / 4.0 |
  | % pixels lit (view only) | 25 / 83 / 82 | 11 / 19 / 22 |

  The C output differs from the numpy mockup on 1.0 to 1.8% of pixels on average (box filter
  rounding), so the mockup sheets are a fair picture of what the board code draws.
  Nano flickers slightly more than layers (the run edges move as the view moves) but every
  pixel means something: lines are geometry, filled areas are walls, solid shapes are sprites.
  The high lit share on E1M3/E1M7 is from close, bright walls filling the view: they quantize
  to the top level and show as solid white with the sprites cut out by their halo.
* **Cost (not measured on the board yet):** a 37 KB memset (marker fill) and one extra area
  average per frame, plus the per column run pass. The log line now prints `walls` and
  `sprites` separately and `styled n/100`.
* **Known limits:** with a shrunk view (screen size below 10) the border inside the crop counts
  as wall. The first wipe frame glitch (see "Layers style") applies here too.
* **Alternatives not taken:** reading the wall columns from the engine's draw segments (more
  engine code to touch); a raycaster-style
  re-render from the map (would no longer be Doom's renderer); drawing the textures with the
  same flat look (the textures are what made the outline style noisy).
* **Static analysis on the branch:** cppcheck is clean on our code, including
  `i_sprite_ids.c` (checked with the engine headers; its only finding is
  `shiftTooManyBitsSigned` inside PrBoom's `m_fixed.h`, third party, left as is). clang-tidy with the checks listed under
  Testing reports `bugprone-easily-swappable-parameters` on x/y helper pairs and
  `readability-function-cognitive-complexity` on `op_layers` (both already present on main,
  accepted: x/y order is consistent everywhere, and `op_layers` is one readable loop).
  `op_nano`'s `bits` parameter carries a NOLINT because it is written through a context
  struct, which `readability-non-const-parameter` does not see.

### Nano sprites (branch `doom-nano`): monsters, shooting, hits
* **Why.** On the board the nano style looked good, walls included; the next step was to make
  the people, the shooting and the hits better. That was a
  visual check on the panel; no numbers were recorded. The first version drew every sprite
  pixel with gray > 40 as on. Scratch mockups on combat captures showed three problems. Dark
  monsters and barrels came out as scattered speckles. On bright walls a monster was white on
  white, kept apart only by its halo. The gun merged with a monster standing behind it.
* **What the engine now tells the platform (patches 4 to 7).** Each sprite gets an id in
  drawing order, so a larger id is nearer. The platform (`i_sprite_ids.c`) writes that id over
  every column post the sprite draws, into a map of the crop window. It reuses the 37 KB PSRAM
  buffer that the layers style uses for its snapshot. The id's class comes from the mobj:
  `op_nano_classify` takes flags only, so it is host tested. The weapon is always id 255. Posts
  drawn outside a sprite are masked mid textures (grates, bars). Each run of them between two
  sprites gets its own id with class MID, which keeps the back-to-front order. On the view
  grid, `op_mask_coverage_cfg` uses the map as is (nonzero means covered), and `op_mask_ids`
  keeps the largest id under each OLED pixel.
* **Classes and looks** (`op_nano`):
  - monster and other things: **adaptive contrast.** Count the lit share of the already drawn
    walls in a ring 3 px around the sprite's box. On a dark background the sprite gets a light
    body: gray > 24 is on, the rim is lit, and pixels darker than 0.6 x the mean of their 5x5
    same-sprite neighbours are cut out (eyes, mouth, gun). On a mostly lit background it gets a
    dark body with a lit rim, and pixels brighter than 150 stay lit. **Hysteresis:** the mode
    is kept per mobj. A light sprite turns dark above 62% lit, and turns back below 38%. A new
    sprite decides at 50%. On the captures this cut contrast mode flips from 355 to 9
    (deadly_corridor), 122 to 15 (defend_the_center), 26 to 12 (E1M1) and 42 to 31 (E1M2);
  - **hit flash:** after `P_DamageMobj` hits a monster (`MF_COUNTKILL` or a lost soul, never
    a player), it is drawn in the opposite mode for 6 tics, and for at least 2 drawn frames.
    At a low frame rate several tics pass per frame, and a tic-only timer could expire
    unseen;
  - **death:** `MF_CORPSE` is set when the death animation starts. Until the body reaches a
    state with `tics == -1`, it keeps the hit look, so a kill reads as a flash. After that it
    is a **corpse:** a 4/16 Bayer dot fill plus rim (inverted on bright walls), so the dead
    recede. Dead-body decorations placed by the map have no `MF_CORPSE` and draw as things;
  - **item** (`MF_SPECIAL`): rim plus pixels brighter than 60. A rim-only version was tried
    first; on 2 to 4 px items it is just a blob that changes shape;
  - **fx** (missiles, bullet puffs, blood): solid on, so shots and hits are visible marks;
  - **weapon:** Bayer dither of gray x 18/16 plus a rim, and pixels brighter than 170 (the
    muzzle flash) solid. Before, the gun was a flat blob;
  - **mid texture:** on if gray > 24, no halo;
  - **halo:** a pixel goes black if one of its 8 neighbours has a higher id that is not a
    mid texture. Every sprite is cut out of the walls behind it, and also out of farther
    sprites. So the gun stays apart from a monster behind it, and so do two overlapping
    monsters.
* **Spectres.** Fuzz-drawn world sprites (`MF_SHADOW`) get no id and stay as hard to see as
  in Doom. A rim would give them away. The weapon keeps its id even with the invisibility
  power.
* **Per mobj state.** A 64 entry table (pointer, last seen tic, hit timer, mode). Pointers are
  only compared, never dereferenced. A full table evicts the least recently seen entry. The
  table is cleared when `leveltime` goes back (new level, loaded game). Zone memory is reused,
  so a new mobj at a recently freed address can inherit a mode or a hit flash for a moment.
  That only affects the look.
* **Measured on host** (C code through ctypes on ViZDoom captures; committed nano vs this):

  | % of pixels that change between frames | E1M1 | E1M2 | deadly_corridor | defend_the_center |
  |---|---|---|---|---|
  | whole view | 12.5 / 12.7 | 8.8 / 9.2 | 11.2 / 11.4 | 2.1 / 2.1 |
  | sprite pixels | 18.7 / 22.7 | 11.8 / 16.3 | 31.3 / 33.2 | 20.3 / 21.3 |
  | weapon pixels | 18.2 / 21.8 | 10.3 / 12.6 | 2.2 / 2.3 | 15.3 / 15.9 |

  The whole view changes almost the same. Sprite pixels change more, mostly on things and
  items (E1M2 things 28.8 -> 48.1). Strips of consecutive frames show why: the committed style
  draws a dark barrel or gibs as a few speckles, because few pixels pass gray > 40. The new
  one draws the barrel and its outline, so more pixels exist to change as the view moves. This
  is a tradeoff I accepted, and it is not yet confirmed on the panel. Two rules were tried
  against it and changed nothing measurable (filled items, no detail cutting on small
  sprites); only the filled items were kept.
* **Limits of that comparison:** ViZDoom labels instead of the engine hooks. Drawing order was
  guessed from on-screen size. Hits were simulated: a new Blood object marks the nearest
  monster. Hit and corpse looks on the sheet are forced by class. The arena wall masks come
  from a depth heuristic (their floors are labelled like walls). ViZDoom keeps the live class
  name on dying monsters, so the death sequence was never captured. The defend_the_line
  scenario was dropped: one "DoomImpBall" label covers 21k pixels of the frame there, and its
  far imps have no labels. The real ordering, hits and deaths are checked only on the board.
* **On the board (2026-10-20):** flashed (from the doom-esp32 repo) and seen on the panel; it
  looked good. That is a visual check of the overall look only. No FPS or mask
  times were recorded, and the hit flash and death sequence were not checked one by one.
* **Parameters** (ring 3 px, 62/38/50%, detail 0.6 over 5x5, highlight 150, weapon x18/16 and
  170, corpse 4/16, hit 6 tics, 64 slots) were chosen from the mockups and have not been
  reviewed on the panel one by one yet.
* **Cost (not measured on the board yet):** the id map replaces the snapshot copy with a 37 KB
  memset, plus one byte write per drawn sprite pixel inside the crop. `op_mask_ids` does one
  max pass. **The log's `sprites` field does not include the per-post writes:** they happen
  inside the engine's drawing and land in the "engine" remainder. Those writes go column by
  column (320 byte stride) into PSRAM, the access pattern that keeps the framebuffer in
  internal RAM. So compare total `frame_us` / fps before and after, not `sprites`. If fps drops
  noticeably, the next step is moving the id map to internal RAM (37 KB; 86 KB is free).
  Internal RAM added, from `nm -S` on the ELF: 10,240 bytes (`s_ids` 8,192, `s_slots` 1,024,
  and `s_modes`, `s_cls`, `s_prev`, `s_id_slot` 256 each). `idf.py size` reports 86,657 bytes
  of DIRAM free.
* **Alternatives not taken:** a sprite mask only (no ids). Without ids the gun and a monster
  behind it are one shape, and there is no class to style by. Contrast from the sprite's own
  colours instead of its background: a red imp on a red wall would still vanish. Flashing the
  whole screen on a hit: Doom already tints the palette when the player is hurt; a monster
  hit needs to show on the monster.

### Not done / open
* Lumps point into read-only flash. No engine code casts a lump to a writable pointer (grep
  for non-const casts on `W_CacheLump*`/`W_LockLump*`: no hits), and the build shows no
  discarded-qualifier warnings, so no write is expected. If one exists it will fault with a
  LoadStore/StoreProhibited exception at an address in 0x3C000000..0x3DFFFFFF.
* `-timedemo demo1` support in this fork is unchecked. If it works, it gives a standard FPS.
* Question for the drosophila session (through the root STATUS.md): does "the rendered frame"
  for LUM_LEFT/LUM_RIGHT include Doom's 32 row status bar?
