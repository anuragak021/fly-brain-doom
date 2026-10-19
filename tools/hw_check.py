"""Hardware check of the brain firmware: link, numeric parity with Python, and timing.

Sends the same Q15 features to the board and to the Python model and reports the largest
activity difference, action disagreements, the board's DWT cycle counts converted to
microseconds, and host-measured round trips.

    python -m tools.hw_check [--port /dev/ttyACM2] [--frames 300]
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

import numpy as np

from game import brain_protocol as bp
from game.controller import PythonController, decode_action, measure_rest
from game.stm32_backend import SerialLink, Stm32Controller, find_stlink_port
from model.rate_model import load_graph

DEFAULT_GRAPH = Path("connectome/data/malecns_v1_0_subgraph.json")
ABS_TOL = 1e-5
REL_TOL = 1e-4


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--port", default=None)
    parser.add_argument("--frames", type=int, default=300)
    parser.add_argument("--graph", type=Path, default=DEFAULT_GRAPH)
    args = parser.parse_args(argv)

    brain = Stm32Controller(SerialLink(args.port or find_stlink_port()))
    info = brain.ping()
    print(
        f"INFO: v{info.proto_version}, {info.n_neurons} neurons, {info.substeps} substeps, "
        f"core clock {info.core_clock_khz} kHz"
    )

    py = PythonController(load_graph(args.graph))
    rest = measure_rest(py)
    py.reset()
    brain.reset()
    rng = np.random.default_rng(11)
    max_diff, action_mismatch, out_of_tol = 0.0, 0, 0
    for _ in range(args.frames):
        feats = bp.quantize_features(rng.random(bp.N_FEATURES).astype(np.float32))
        want = py.step(feats)
        got = brain.step(feats)
        pairs = zip(
            (got.steer_left, got.steer_right, got.forward, got.backward),
            (want.steer_left, want.steer_right, want.forward, want.backward),
        )
        for a, b in pairs:
            max_diff = max(max_diff, abs(a - b))
            out_of_tol += int(abs(a - b) > ABS_TOL + REL_TOL * abs(b))
        tl, tr = decode_action(want, rest)
        expected = (bp.ACTION_TURN_LEFT if tl else 0) | (
            bp.ACTION_TURN_RIGHT if tr else 0
        )
        action_mismatch += int(brain.last_actions != expected)

    cycles = np.array(brain.cycles)
    us = cycles / (info.core_clock_khz / 1000.0)
    trips = np.array(brain.round_trips_s[-args.frames :]) * 1000.0
    print(
        f"frames: {args.frames}; max |board - python| activity difference: {max_diff:.3e}"
    )
    print(
        f"values outside tolerance: {out_of_tol}; action mismatches: {action_mismatch}; misses: {brain.misses}"
    )
    print(
        f"model step on the board (DWT): min {us.min():.1f} us, mean {us.mean():.1f} us, "
        f"max {us.max():.1f} us ({int(cycles.mean())} cycles mean at {info.core_clock_khz / 1000:.0f} MHz)"
    )
    print(
        f"STEP round trip (host-measured, includes USB serial): mean {trips.mean():.2f} ms, "
        f"p95 {np.percentile(trips, 95):.2f} ms, max {trips.max():.2f} ms"
    )
    return 0 if (out_of_tol == 0 and action_mismatch == 0) else 1


if __name__ == "__main__":
    sys.exit(main())
