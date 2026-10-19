"""ViZDoom game loop driven by a Controller backend.

Run from the repository root:
    python -m game.play --backend python --episodes 3

Action mapping (defend_the_center buttons: TURN_LEFT, TURN_RIGHT, ATTACK):
  * turn left / right : decoded from the controller's steering outputs (connectome driven)
  * attack            : HEURISTIC, not connectome driven. Fires whenever an enemy is
                        visible within FIRE_CONE of screen centre.
  * forward / backward activities are logged only (this scenario has no move buttons).
"""

from __future__ import annotations

import argparse
import sys
import time
from pathlib import Path

import numpy as np

from game.controller import Controller, PythonController, decode_action, measure_rest
from game.features import Label, make_features
from model.rate_model import load_graph

DEFAULT_GRAPH = Path("connectome/data/malecns_v1_0_subgraph.json")
FIRE_CONE = 0.15  # |enemy offset| below this counts as aimed (heuristic fire rule)
FRAME_SKIP = 2  # game tics per decision


def make_controller(backend: str, graph_path: Path) -> Controller:
    """Create the requested backend."""
    if backend == "python":
        return PythonController(load_graph(graph_path))
    raise NotImplementedError("backend 'stm32' needs the Nucleo and is Phase D")


def make_game(scenario: str, visible: bool):
    """Configure and start a ViZDoom game with grey 160x120 screen and labels."""
    import vizdoom as vzd

    game = vzd.DoomGame()
    game.load_config(str(Path(vzd.scenarios_path) / f"{scenario}.cfg"))
    game.set_window_visible(visible)
    game.set_screen_resolution(vzd.ScreenResolution.RES_160X120)
    game.set_screen_format(vzd.ScreenFormat.GRAY8)
    game.set_labels_buffer_enabled(True)
    game.add_available_game_variable(vzd.GameVariable.KILLCOUNT)
    game.init()
    return game


def read_labels(state) -> list[Label]:
    """Convert ViZDoom labels to the plain Label tuples features.py expects."""
    return [
        Label(lb.object_name, lb.x, lb.y, lb.width, lb.height) for lb in state.labels
    ]


def play_episode(game, controller: Controller, rest, rng: np.random.Generator) -> dict:
    """Play one episode; return kills, steps and mean controller step time."""
    game.new_episode()
    controller.reset()
    ctrl_time, frames, seen, toward, away = 0.0, 0, 0, 0, 0
    while not game.is_episode_finished():
        state = game.get_state()
        if state is None:
            break
        feat, offset = make_features(state.screen_buffer, read_labels(state))
        t0 = time.perf_counter()
        out = controller.step(feat)
        ctrl_time += time.perf_counter() - t0
        left, right = decode_action(out, rest)
        if offset is not None and abs(offset) > FIRE_CONE:
            seen += 1
            toward += int((offset < 0 and left) or (offset > 0 and right))
            away += int((offset < 0 and right) or (offset > 0 and left))
        fire = offset is not None and abs(offset) < FIRE_CONE
        game.make_action([left, right, fire], FRAME_SKIP)
        frames += 1
    kills = int(game.get_game_variable(__import__("vizdoom").GameVariable.KILLCOUNT))
    return {
        "kills": kills,
        "frames": frames,
        "seen": seen,
        "toward": toward,
        "away": away,
        "ctrl_ms": 1000.0 * ctrl_time / max(frames, 1),
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--backend", choices=("python", "stm32"), default="python")
    parser.add_argument("--graph", type=Path, default=DEFAULT_GRAPH)
    parser.add_argument("--scenario", default="defend_the_center")
    parser.add_argument("--episodes", type=int, default=3)
    parser.add_argument("--window", action="store_true", help="show the game window")
    args = parser.parse_args(argv)

    controller = make_controller(args.backend, args.graph)
    rest = measure_rest(controller)
    game = make_game(args.scenario, args.window)
    rng = np.random.default_rng(0)
    t_start = time.perf_counter()
    total_frames = 0
    for ep in range(args.episodes):
        res = play_episode(game, controller, rest, rng)
        total_frames += res["frames"]
        print(
            f"episode {ep}: kills={res['kills']} decisions={res['frames']} controller={res['ctrl_ms']:.3f} ms/step "
            f"off-centre-enemy frames={res['seen']} toward={res['toward']} away={res['away']}"
        )
    elapsed = time.perf_counter() - t_start
    print(
        f"loop rate: {total_frames / elapsed:.1f} decisions/s over {args.episodes} episodes"
    )
    game.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
