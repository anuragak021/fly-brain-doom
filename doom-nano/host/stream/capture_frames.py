"""Capture 320x200 palette-index Doom frames from ViZDoom (same format as PrBoom's framebuffer).

Writes an .npz with `frames` (N, 200, 320) uint8 and `palette` (256, 3) uint8.
Examples:
    .venv/bin/python host/stream/capture_frames.py --scenario freedoom1 --frames 300 \
        --out host/captures/e1m1.npz
    # the board's IWAD and one of the maps its attract demos play
    .venv/bin/python host/stream/capture_frames.py --iwad wad/doom1.wad --map E1M5 \
        --frames 300 --out host/captures/doom1_e1m5.npz
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

import numpy as np
import vizdoom as vzd

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from oledpipe import read_playpal  # noqa: E402

# Scenario name -> IWAD whose PLAYPAL the frames use (the scenario's doom_game_path).
SCENARIOS = {
    "freedoom1": "freedoom1.wad",
    "deathmatch": "freedoom2.wad",
    "defend_the_center": "freedoom2.wad",
}


def make_game(
    scenario: str, seed: int, iwad: Path | None, doom_map: str
) -> vzd.DoomGame:
    game = vzd.DoomGame()
    game.load_config(str(Path(vzd.scenarios_path) / f"{scenario}.cfg"))
    if iwad is not None:
        game.set_doom_game_path(str(iwad.resolve()))
        game.set_doom_map(doom_map)
    game.set_screen_format(vzd.ScreenFormat.DOOM_256_COLORS8)
    game.set_screen_resolution(vzd.ScreenResolution.RES_320X200)
    game.set_render_hud(
        True
    )  # keep the status bar, as PrBoom draws it; the crop removes it
    game.set_window_visible(False)
    game.set_mode(vzd.Mode.PLAYER)
    game.set_seed(seed)
    game.clear_available_buttons()
    for button in (
        vzd.Button.MOVE_FORWARD,
        vzd.Button.TURN_LEFT,
        vzd.Button.TURN_RIGHT,
        vzd.Button.ATTACK,
    ):
        game.add_available_button(button)
    game.set_episode_timeout(0)
    game.init()
    return game


def capture(
    scenario: str, n_frames: int, skip: int, seed: int, iwad: Path | None, doom_map: str
) -> np.ndarray:
    """Wander with a simple scripted policy: mostly forward, turning in bursts, firing at times."""
    rng = np.random.default_rng(seed)
    game = make_game(scenario, seed, iwad, doom_map)
    frames: list[np.ndarray] = []
    turn = [0, 0]
    turn_left_tics = 0
    try:
        while len(frames) < n_frames:
            if game.is_episode_finished():
                game.new_episode()
            if turn_left_tics == 0:
                turn = [1, 0] if rng.random() < 0.5 else [0, 1]
                turn_left_tics = int(rng.integers(3, 12)) if rng.random() < 0.4 else 8
                if rng.random() < 0.6:
                    turn = [0, 0]
            turn_left_tics -= 1
            action = [1, turn[0], turn[1], int(rng.random() < 0.1)]
            game.make_action(action, skip)
            state = game.get_state()
            if state is not None:
                frames.append(state.screen_buffer.copy())
    finally:
        game.close()
    return np.stack(frames)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--scenario", choices=sorted(SCENARIOS), default="freedoom1")
    parser.add_argument("--frames", type=int, default=200, help="frames to keep")
    parser.add_argument(
        "--skip", type=int, default=2, help="game tics per captured frame"
    )
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument(
        "--iwad",
        type=Path,
        help="play --map of this IWAD instead of the scenario's (e.g. wad/doom1.wad, the board's)",
    )
    parser.add_argument(
        "--map",
        default="E1M1",
        help="map for --iwad; the shareware attract demos play E1M5, E1M3 and E1M7",
    )
    args = parser.parse_args()
    if args.frames <= 0 or args.skip <= 0:
        parser.error("--frames and --skip must be positive")

    if args.iwad is not None and not args.iwad.is_file():
        parser.error(f"--iwad {args.iwad} not found")
    frames = capture(
        args.scenario, args.frames, args.skip, args.seed, args.iwad, args.map
    )
    iwad = (
        args.iwad
        if args.iwad is not None
        else Path(vzd.__file__).parent / SCENARIOS[args.scenario]
    )
    palette = read_playpal(iwad)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    np.savez_compressed(args.out, frames=frames, palette=palette)
    print(f"saved {frames.shape[0]} frames {frames.shape[1:]} to {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
