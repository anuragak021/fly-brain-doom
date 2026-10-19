"""ViZDoom game loop driven by a Controller backend.

Run from the repository root:
    python -m game.play --backend python --episodes 3
    python -m game.play --backend stm32 --episodes 3          # board on the ST-Link VCP

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

from game.brain_protocol import ACTION_TURN_LEFT, ACTION_TURN_RIGHT, quantize_features
from game.controller import Controller, PythonController, decode_action, measure_rest
from game.features import Label, make_features
from model.rate_model import load_graph

DEFAULT_GRAPH = Path("connectome/data/malecns_v1_0_subgraph.json")
FIRE_CONE = 0.15  # |enemy offset| below this counts as aimed (heuristic fire rule)
FRAME_SKIP = 2  # game tics per decision


def make_controller(backend: str, graph_path: Path, port: str | None) -> Controller:
    """Create the requested backend."""
    if backend == "python":
        return PythonController(load_graph(graph_path))
    from game.stm32_backend import SerialLink, Stm32Controller, find_stlink_port

    controller = Stm32Controller(SerialLink(port or find_stlink_port()))
    info = controller.ping()
    print(
        f"brain online: protocol v{info.proto_version}, {info.n_neurons} neurons, "
        f"{info.substeps} substeps/step, core clock {info.core_clock_khz / 1000:.0f} MHz"
    )
    return controller


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
    ctrl_time, frames, seen, toward, away, mismatch = 0.0, 0, 0, 0, 0, 0
    while not game.is_episode_finished():
        state = game.get_state()
        if state is None:
            break
        feat, offset = make_features(state.screen_buffer, read_labels(state))
        feat = quantize_features(
            feat
        )  # what the brain sees on the wire (Q15), any backend
        t0 = time.perf_counter()
        out = controller.step(feat)
        ctrl_time += time.perf_counter() - t0
        left, right = decode_action(out, rest)
        brain_actions = getattr(controller, "last_actions", None)
        if (
            brain_actions is not None
        ):  # the board decided; cross-check against the Python rule
            hw_left = bool(brain_actions & ACTION_TURN_LEFT)
            hw_right = bool(brain_actions & ACTION_TURN_RIGHT)
            mismatch += int((hw_left, hw_right) != (left, right))
            left, right = hw_left, hw_right
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
        "mismatch": mismatch,
        "ctrl_ms": 1000.0 * ctrl_time / max(frames, 1),
    }


def report_link(controller: Controller) -> None:
    """Print link statistics for the stm32 backend (host-measured round trips, board cycles)."""
    trips = getattr(controller, "round_trips_s", None)
    if not trips:
        return
    ms = np.array(trips) * 1000.0
    print(
        f"link round trip (host-measured, USB serial included): mean {ms.mean():.2f} ms, "
        f"p95 {np.percentile(ms, 95):.2f} ms, max {ms.max():.2f} ms; misses {controller.misses}"
    )
    cycles = getattr(controller, "cycles", [])
    if cycles:
        print(
            f"board step cycles: min {min(cycles)}, mean {np.mean(cycles):.0f}, max {max(cycles)}"
        )


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--backend", choices=("python", "stm32"), default="python")
    parser.add_argument("--graph", type=Path, default=DEFAULT_GRAPH)
    parser.add_argument("--scenario", default="defend_the_center")
    parser.add_argument("--episodes", type=int, default=3)
    parser.add_argument("--window", action="store_true", help="show the game window")
    parser.add_argument(
        "--port", default=None, help="serial port for --backend stm32 (default: auto)"
    )
    args = parser.parse_args(argv)

    controller = make_controller(args.backend, args.graph, args.port)
    # Resting reference always comes from the Python model, so the board's decode is cross-checked.
    rest = measure_rest(PythonController(load_graph(args.graph)))
    game = make_game(args.scenario, args.window)
    rng = np.random.default_rng(0)
    t_start = time.perf_counter()
    total_frames = 0
    for ep in range(args.episodes):
        res = play_episode(game, controller, rest, rng)
        total_frames += res["frames"]
        print(
            f"episode {ep}: kills={res['kills']} decisions={res['frames']} controller={res['ctrl_ms']:.3f} ms/step "
            f"off-centre-enemy frames={res['seen']} toward={res['toward']} away={res['away']} "
            f"decode-mismatch={res['mismatch']}"
        )
    elapsed = time.perf_counter() - t_start
    print(
        f"loop rate: {total_frames / elapsed:.1f} decisions/s over {args.episodes} episodes"
    )
    game.close()
    report_link(controller)
    return 0


if __name__ == "__main__":
    sys.exit(main())
