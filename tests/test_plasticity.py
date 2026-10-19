"""Host tests for the Python plasticity reference (model/plasticity.py).

The C twin is checked against this reference by tests/c/test_plasticity.c; here the rule's
own invariants are checked: which weights may change, sign and size bounds, row sums, reset.
"""

from __future__ import annotations

from pathlib import Path

import numpy as np

from game import controller as ctl
from model import export_c
from model import plasticity as pl
from model.rate_model import load_graph

REAL_GRAPH = (
    Path(__file__).resolve().parent.parent
    / "connectome/data/malecns_v1_0_subgraph.json"
)


def _learning_run(frames: int, reward_every: int) -> tuple[pl.Plasticity, np.ndarray]:
    graph = load_graph(REAL_GRAPH)
    controller = ctl.PythonController(graph)
    model = controller._model
    model.w = model.w.copy()
    w0 = model.w.copy()
    learner = export_c.make_learner(graph, model.w)
    rng = np.random.default_rng(3)
    controller.reset()
    for i in range(frames):
        out = controller.step(rng.random(ctl.N_FEATURES).astype(np.float32))
        reward = 1.0 if i % reward_every == 0 else 0.0
        learner.update(model.r, out.steer_left, out.steer_right, reward)
    return learner, w0


def test_only_steering_rows_change_and_bounds_hold() -> None:
    learner, w0 = _learning_run(150, 7)
    assert learner.n_learnable == 320
    assert learner.drift() > 0.0
    changed_rows = set(np.nonzero(np.any(learner.w != w0, axis=1))[0].tolist())
    assert changed_rows <= set(learner.rows)
    for n, (i, c) in enumerate(zip(learner.rows, learner.cols)):
        w, orig = learner.w[i, c], w0[i, c]
        assert np.all(w * orig >= 0)  # Dale's law: no sign flips
        assert np.all(
            np.abs(w) <= np.float32(pl.WEIGHT_MAX_FACTOR) * np.abs(orig)
        )  # exact cap
        assert float(np.abs(w).sum()) <= float(learner.abs0[n]) + 1e-4  # scaling
        assert np.all(learner.w[i, w0[i] == 0] == 0)  # no new synapses


def test_reset_weights_restores_connectome() -> None:
    learner, w0 = _learning_run(60, 5)
    assert learner.drift() > 0.0
    learner.reset_weights()
    assert learner.drift() == 0.0
    assert np.array_equal(learner.w, w0)
    assert (learner.ref_left, learner.ref_right) == learner.rest0


def test_more_reward_events_move_weights_further() -> None:
    """One reward event in 80 frames moves the weights less than a reward every frame."""
    learner, _ = _learning_run(80, 10**9)  # only frame 0 is rewarded
    first_only = learner.drift()
    learner2, _ = _learning_run(
        80, 1
    )  # every frame rewarded: the mean absorbs it slowly
    assert first_only < learner2.drift()
