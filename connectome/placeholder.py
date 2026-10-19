"""Generate a small PLACEHOLDER graph with the same schema as the real extraction.

THIS IS NOT CONNECTOME DATA. It is a hand-made random graph so that the model,
game loop and C export can be built and tested while real data access is pending.
The output file is marked ``"placeholder": true`` and must never be described as
connectome derived.

Schema (JSON):
  meta:    provenance and thresholds, plus ``placeholder`` flag
  neurons: list of {idx, bodyId, type, side, nt, role}; role in input/hidden/output
  edges:   list of {pre, post, count, sign}; pre/post are neuron idx values
Weight convention: W[post, pre], sign belongs to the presynaptic neuron.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import numpy as np

N_INPUT_PER_SIDE = 12
N_HIDDEN_PER_SIDE = 40
OUTPUT_TYPES = ("steer", "steer", "forward")  # left, right, forward
EDGE_PROB = 0.08
INHIBITORY_FRACTION = 0.25
MAX_COUNT = 60
SEED = 1234


def build_placeholder(seed: int = SEED) -> dict:
    """Return the placeholder graph as a JSON-serialisable dict."""
    rng = np.random.default_rng(seed)
    neurons: list[dict] = []

    def add(role: str, ntype: str, side: str) -> None:
        nt = "gaba" if rng.random() < INHIBITORY_FRACTION else "acetylcholine"
        neurons.append(
            {
                "idx": len(neurons),
                "bodyId": -(len(neurons) + 1),  # negative = not a real body id
                "type": ntype,
                "side": side,
                "nt": nt,
                "role": role,
            }
        )

    for side in ("L", "R"):
        for _ in range(N_INPUT_PER_SIDE):
            add("input", "PLACEHOLDER_IN", side)
    for side in ("L", "R"):
        for _ in range(N_HIDDEN_PER_SIDE):
            add("hidden", "PLACEHOLDER_HID", side)
    for ntype, side in zip(OUTPUT_TYPES, ("L", "R", "M")):
        add("output", f"PLACEHOLDER_{ntype.upper()}", side)

    n = len(neurons)
    by_role = {
        r: [x["idx"] for x in neurons if x["role"] == r]
        for r in ("input", "hidden", "output")
    }
    edges: list[dict] = []
    for post in by_role["hidden"] + by_role["output"]:
        for pre in by_role["input"] + by_role["hidden"]:
            if pre != post and rng.random() < EDGE_PROB:
                sign = -1 if neurons[pre]["nt"] == "gaba" else 1
                edges.append(
                    {
                        "pre": pre,
                        "post": post,
                        "count": int(rng.integers(5, MAX_COUNT)),
                        "sign": sign,
                    }
                )

    return {
        "meta": {
            "placeholder": True,
            "source": "PLACEHOLDER random graph, not connectome data",
            "seed": seed,
            "n_neurons": n,
            "n_edges": len(edges),
            "weight_convention": "W[post, pre], sign by presynaptic neuron",
        },
        "neurons": neurons,
        "edges": edges,
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--out", type=Path, default=Path("connectome/data/placeholder_graph.json")
    )
    parser.add_argument("--seed", type=int, default=SEED)
    args = parser.parse_args(argv)
    graph = build_placeholder(args.seed)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(graph, indent=1))
    meta = graph["meta"]
    print(
        f"wrote PLACEHOLDER graph: {meta['n_neurons']} neurons, {meta['n_edges']} edges -> {args.out}"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
