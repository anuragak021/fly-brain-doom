"""Extract a sensorimotor subgraph from the MaleCNS connectome via neuPrint.

Token: read from the NEUPRINT_APPLICATION_CREDENTIALS environment variable by
the neuprint client. It is never passed on a command line or written to disk.

Method (see NOTES.md for the reasoning):
  1. Output neurons: descending neurons by type (OUTPUT_GROUPS).
  2. Hop 1: all upstream partners of the outputs with weight >= HOP1_MIN_WEIGHT.
  3. Hop 2: upstream partners of hop 1, ranked by total synapses into hop 1,
     truncated so the whole network has at most MAX_NEURONS neurons.
  4. Induced subgraph: all connections among kept neurons (recurrent and
     lateral edges included), ROI rows summed per neuron pair.
  5. Signs from predicted neurotransmitter, per presynaptic neuron.
  6. Input neurons: visual neurons if the walk reached any, else the hop-2
     layer (documented fallback).

Weight convention: W[post, pre], sign belongs to the presynaptic neuron.
"""

from __future__ import annotations

import argparse
import json
import sys
from datetime import date
from pathlib import Path

import pandas as pd
from neuprint import (
    Client,
    fetch_adjacencies,
    fetch_neurons,
    fetch_simple_connections,
)
from neuprint import NeuronCriteria as NC

SERVER = "neuprint.janelia.org"
DATASET = "male-cns:v1.0"

# role label -> neuron types. DNp36 stands in for P9 (synonym "pIP9"); unverified.
OUTPUT_GROUPS: dict[str, tuple[str, ...]] = {
    "steer": ("DNa01", "DNa02"),
    "forward": ("DNp36",),
    "backward": ("MDN",),
}
HOP1_MIN_WEIGHT = 10
HOP2_MIN_WEIGHT = 10
MAX_NEURONS = 300
HOP1_MAX = 100
ADJ_MIN_WEIGHT = 5
NT_CONFIDENCE_MIN = 0.5
NT_SIGN = {"acetylcholine": 1, "gaba": -1, "glutamate": -1}
VISUAL_SUPERCLASSES = ("visual_projection", "visual_centrifugal", "optic")


def fetch_outputs() -> pd.DataFrame:
    """Return the output neurons with their group label; fail if a type is missing."""
    frames = []
    for group, types in OUTPUT_GROUPS.items():
        for ntype in types:
            df, _ = fetch_neurons(NC(type=ntype))
            if df.empty:
                raise RuntimeError(f"output type {ntype!r} not found in {DATASET}")
            df = df.assign(group=group)
            frames.append(df)
    return pd.concat(frames, ignore_index=True)


def upstream_ranked(targets: list[int], min_weight: int) -> pd.Series:
    """Total synapses from each upstream neuron into ``targets``, descending."""
    conn = fetch_simple_connections(
        upstream_criteria=None,
        downstream_criteria=NC(bodyId=targets),
        min_weight=min_weight,
    )
    if conn.empty:
        return pd.Series(dtype="int64")
    return conn.groupby("bodyId_pre")["weight"].sum().sort_values(ascending=False)


def walk_upstream(outputs: list[int]) -> tuple[set[int], set[int]]:
    """Return (hop1, hop2) body id sets, hop2 truncated to the neuron cap."""
    hop1_rank = upstream_ranked(outputs, HOP1_MIN_WEIGHT)
    hop1_rank = hop1_rank[~hop1_rank.index.isin(outputs)]
    hop1 = set(hop1_rank.index[:HOP1_MAX])
    hop2_rank = upstream_ranked(sorted(hop1), HOP2_MIN_WEIGHT)
    hop2_rank = hop2_rank[~hop2_rank.index.isin(hop1 | set(outputs))]
    budget = max(MAX_NEURONS - len(outputs) - len(hop1), 0)
    return hop1, set(hop2_rank.index[:budget])


def classify_nt(row: pd.Series) -> str:
    """Neurotransmitter label for one neuron, or 'unknown' if not confident."""
    nt = row.get("consensusNt")
    if isinstance(nt, str) and nt:
        return nt.lower()
    nt = row.get("predictedNt")
    conf = row.get("predictedNtConfidence")
    if isinstance(nt, str) and nt and pd.notna(conf) and conf >= NT_CONFIDENCE_MIN:
        return nt.lower()
    return "unknown"


def induced_edges(body_ids: list[int]) -> pd.DataFrame:
    """Edges among ``body_ids`` with ROI rows summed per (pre, post) pair."""
    _, conn = fetch_adjacencies(
        sources=body_ids,
        targets=body_ids,
        min_total_weight=ADJ_MIN_WEIGHT,
        properties=["weight"],
    )
    return conn.groupby(["bodyId_pre", "bodyId_post"], as_index=False)["weight"].sum()


def check_pair_totals(edges: pd.DataFrame) -> None:
    """Cross-check the heaviest edge against fetch_simple_connections (fails loudly)."""
    top = edges.sort_values("weight", ascending=False).iloc[0]
    ref = fetch_simple_connections(
        NC(bodyId=[int(top.bodyId_pre)]), NC(bodyId=[int(top.bodyId_post)])
    )
    if int(ref["weight"].sum()) != int(top.weight):
        raise RuntimeError(
            f"edge total mismatch: adjacency {top.weight} vs simple {ref['weight'].sum()}"
        )


def choose_inputs(nodes: pd.DataFrame, hop2: set[int]) -> tuple[set[int], str]:
    """Pick input neurons: visual ones if present, else the hop-2 layer."""
    visual = nodes[nodes["superclass"].astype(str).isin(VISUAL_SUPERCLASSES)]
    if len(visual) > 0:
        return set(visual["bodyId"]), "visual superclass neurons"
    return set(hop2), "FALLBACK: hop-2 layer (no visual neurons reached)"


def build_graph(args: argparse.Namespace) -> dict:
    """Run the full extraction and return the JSON-serialisable graph."""
    outputs = fetch_outputs()
    out_ids = [int(b) for b in outputs["bodyId"]]
    hop1, hop2 = walk_upstream(out_ids)
    keep = sorted(set(out_ids) | hop1 | hop2)
    nodes, _ = fetch_neurons(NC(bodyId=keep))
    nodes = nodes.set_index("bodyId", drop=False)
    nodes["nt"] = nodes.apply(classify_nt, axis=1)
    edges = induced_edges(keep)
    check_pair_totals(edges)
    inputs, input_rule = choose_inputs(nodes.drop(index=out_ids), hop2)
    group_of = dict(zip(outputs["bodyId"], outputs["group"]))
    order = {b: i for i, b in enumerate(keep)}
    neurons = []
    for b in keep:
        r = nodes.loc[b]
        role = "output" if b in group_of else ("input" if b in inputs else "hidden")
        neurons.append(
            {
                "idx": order[b],
                "bodyId": int(b),
                "type": str(r["type"]),
                "side": str(r["somaSide"]),
                "nt": r["nt"],
                "role": role,
                "group": group_of.get(b, ""),
            }
        )
    edge_list = [
        {
            "pre": order[int(e.bodyId_pre)],
            "post": order[int(e.bodyId_post)],
            "count": int(e.weight),
            "sign": NT_SIGN.get(neurons[order[int(e.bodyId_pre)]]["nt"], 0),
        }
        for e in edges.itertuples()
    ]
    meta = {
        "placeholder": False,
        "server": SERVER,
        "dataset": DATASET,
        "queried": date.today().isoformat(),
        "hop1_min_weight": HOP1_MIN_WEIGHT,
        "hop2_min_weight": HOP2_MIN_WEIGHT,
        "adjacency_min_weight": ADJ_MIN_WEIGHT,
        "max_neurons": MAX_NEURONS,
        "nt_confidence_min": NT_CONFIDENCE_MIN,
        "input_rule": input_rule,
        "n_neurons": len(neurons),
        "n_edges": len(edge_list),
        "n_synapses": int(sum(e["count"] for e in edge_list)),
        "n_hop1": len(hop1),
        "n_hop2": len(hop2),
        "weight_convention": "W[post, pre], sign by presynaptic neuron",
    }
    return {"meta": meta, "neurons": neurons, "edges": edge_list}


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--out", type=Path, default=Path("connectome/data/malecns_v1_0_subgraph.json")
    )
    args = parser.parse_args(argv)
    client = Client(
        SERVER, dataset=DATASET, progress=False
    )  # keep alive: default client is weakly held
    graph = build_graph(args)
    del client
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(graph, indent=1))
    print(json.dumps(graph["meta"], indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
