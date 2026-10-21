# Third party code and data

This repository is licensed GPL-2.0-or-later because it links the PrBoom engine (GPL).

| Component | Source | License | Used for | Modified? |
|---|---|---|---|---|
| PrBoom 2.5.0 engine, as adapted in retro-go `prboom-go` (GitHub ducalex/retro-go); authors listed in `components/prboom/AUTHORS` | https://github.com/ducalex/retro-go/tree/4ced120669750ca7228fd0414211430c1d923166/prboom-go/components/prboom | GPL-2.0 (`components/prboom/COPYING`) | Doom engine, in `components/prboom/` | patches marked `/* doom-esp32 patch: ... */`: on main `d_main.c` WAD from flash and `r_things.c` sprite mask hooks; on branch `doom-nano` also `r_main.c` wall mask hooks, `r_things.c` sprite id hooks (`vis->thing`, `I_SpriteThing`/`I_SpritePost`/`I_SpriteDone`) and `p_inter.c` hit hook (seven in all); CMakeLists.txt replaced (build glue). All listed in NOTES.md |
| prboom.wad (compiled in as `components/prboom/prboom_wad.h`) | same | GPL-2.0 | engine resources | no |
| ESP-IDF v5.5.2 | https://github.com/espressif/esp-idf | Apache-2.0 | SDK, drivers, FreeRTOS | no |
| ViZDoom 1.3.2 (pip, host only, not shipped) | https://github.com/Farama-Foundation/ViZDoom | MIT | laptop frame source for previews and the Phase 1 demo | no |
| Freedoom (bundled inside the ViZDoom wheel as freedoom1.wad / freedoom2.wad) | https://freedoom.github.io | BSD-3-Clause | game content rendered in captures and in `docs/preview/*.png` | no |
| doom-nano look (branch `doom-nano` only) | https://github.com/QC20/Doom-run-on-Arduino-Pro-Mini-and-SSD1306 (a copy of https://github.com/daveruiz/doom-nano) | none stated | **design reference only**: the visual style (flat shaded walls, black floors, white sprites, bottom HUD) was rebuilt from scratch in `op_nano.c` / `op_hud.c` | no code, font, sprite or image copied |
| numpy, Pillow, pyserial (host tools) | PyPI, versions pinned in host/requirements.txt | BSD-3-Clause / MIT-CMU / BSD-3-Clause | host tools | no |

## Images in docs/preview
The preview PNGs show frames rendered by ViZDoom from the Freedoom IWADs (Freedoom artwork,
BSD-3-Clause, copyright the Freedoom contributors). They are produced by
`host/preview/preview.py` from captures made with `host/stream/capture_frames.py`.

## Game data (WAD)
No WAD is committed (`*.wad` and `wad/` are gitignored). The board uses the shareware
DOOM1.WAD v1.9 (id Software, freely redistributable shareware; 4,196,020 bytes, md5
f0cefca49926d00903cf57551d901abe), taken from the `data/` folder of the same retro-go commit.
`tools/flash_wad.py` checks it and writes it to the `wad` partition. The Freedoom IWADs are 28.8 MB each, which is larger than the 16 MB
flash, so they cannot be used on the board.

## Original work (written for this project)
Everything outside `components/prboom/` and the data above:
`components/doom_platform` (engine platform layer), `main/`, `components/oled_pipeline`,
`components/oled_driver`, `components/demo_stream`, `demo/oled_stream`, `tools/`, `host/`.
