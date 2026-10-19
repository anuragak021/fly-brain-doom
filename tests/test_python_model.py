"""Host tests for the feature extraction, weight construction and rate model."""

from __future__ import annotations

import numpy as np
import pytest

from game.controller import Outputs, PythonController, decode_action, measure_rest
from game.features import (
    FEATURE_ENEMY_L,
    FEATURE_ENEMY_R,
    N_FEATURES,
    Label,
    enemy_offset,
    luminance_halves,
    make_features,
)
from model.rate_model import RHO_TARGET, RateModel, build_weights, load_graph

REAL_GRAPH = "connectome/data/malecns_v1_0_subgraph.json"


def _tiny_graph() -> dict:
    """Two excitatory neurons feeding one inhibited neuron: 0,1 -> 2 (0 exc, 1 inh)."""
    neurons = [
        {"idx": i, "bodyId": i, "type": "t", "side": "L", "nt": "x", "role": "hidden"}
        for i in range(3)
    ]
    edges = [
        {"pre": 0, "post": 2, "count": 30, "sign": 1},
        {"pre": 1, "post": 2, "count": 10, "sign": -1},
    ]
    return {"meta": {}, "neurons": neurons, "edges": edges}


def test_luminance_halves_left_bright() -> None:
    img = np.zeros((4, 6), dtype=np.uint8)
    img[:, :3] = 255
    left, right = luminance_halves(img)
    assert left == pytest.approx(1.0)
    assert right == pytest.approx(0.0)


def test_luminance_rejects_bad_shape() -> None:
    with pytest.raises(ValueError):
        luminance_halves(np.zeros((4,), dtype=np.uint8))


def test_enemy_offset_picks_largest_enemy_and_ignores_others() -> None:
    labels = [
        Label("DoomPlayer", 0, 0, 100, 100),
        Label("Zombieman", 10, 0, 10, 10),  # small, left
        Label("Zombieman", 140, 0, 20, 20),  # large, right
    ]
    off = enemy_offset(labels, 160)
    assert off is not None and off > 0.8


def test_enemy_offset_none_without_enemy() -> None:
    assert enemy_offset([Label("Medikit", 0, 0, 5, 5)], 160) is None


def test_features_enemy_left_sets_left_channel() -> None:
    gray = np.zeros((8, 160), dtype=np.uint8)
    feat, off = make_features(gray, [Label("Zombieman", 10, 0, 10, 10)])
    assert feat.shape == (N_FEATURES,) and feat.dtype == np.float32
    assert off is not None and off < 0
    assert feat[FEATURE_ENEMY_L] > 0 and feat[FEATURE_ENEMY_R] == 0


def test_weight_convention_post_pre_and_signs() -> None:
    w = build_weights(_tiny_graph())
    assert w[2, 0] > 0 > w[2, 1]  # row = post, column = pre
    assert w[0, 2] == 0 and w[2, 2] == 0


def test_weights_are_stable_by_construction() -> None:
    w = load_graph(REAL_GRAPH).weights
    rho = float(np.max(np.abs(np.linalg.eigvals(np.abs(w)))))
    assert rho == pytest.approx(RHO_TARGET, abs=1e-4)


def test_rate_model_bounded_and_nonnegative() -> None:
    w = load_graph(REAL_GRAPH).weights
    model = RateModel(w)
    drive = np.full(w.shape[0], 1.0, dtype=np.float32)
    for _ in range(200):
        r = model.step(drive)
    assert np.all(r >= 0) and np.all(np.isfinite(r))
    assert float(r.max()) <= 1.0 / (1.0 - RHO_TARGET) + 1e-3


def test_left_input_turns_left_and_right_input_turns_right() -> None:
    ctrl = PythonController(load_graph(REAL_GRAPH))
    rest = measure_rest(ctrl)
    for feats, expected in (
        ([0.0, 0.0, 0.8, 0.0, 0.0, 0.0], (True, False)),
        ([0.0, 0.0, 0.0, 0.8, 0.0, 0.0], (False, True)),
        ([0.0, 0.0, 0.0, 0.0, 0.8, 0.0], (True, False)),  # hit from the left
        ([0.0, 0.0, 0.0, 0.0, 0.0, 0.8], (False, True)),
    ):
        ctrl.reset()
        out = rest
        for _ in range(15):
            out = ctrl.step(np.array(feats, dtype=np.float32))
        assert decode_action(out, rest) == expected


def test_no_input_gives_no_turn() -> None:
    ctrl = PythonController(load_graph(REAL_GRAPH))
    rest = measure_rest(ctrl)
    assert decode_action(rest, rest) == (False, False)
    assert isinstance(rest, Outputs)
