"""Reward-modulated plasticity on the steering neurons' input synapses (Python reference).

The C version (firmware/src/plasticity.c) must match this step for step; the golden file
tests/golden/learn_golden.csv, written by model/export_c.py, checks that.

Once per game frame, after the rate model has run (only while learning is switched on):

    e_k  <- TRACE_DECAY * e_k + r_pre(k) * (r_post(k) - b_post(k))      eligibility trace
    b_i  <- b_i + BASELINE_RATE * (r_i - b_i)                         running mean of post
    R'   <- reward - R_mean;  R_mean <- R_mean + REWARD_MEAN_RATE * (reward - R_mean)
    w_k  <- w_k + LEARN_RATE * R' * e_k * |w0_k| + FORGET_RATE * (w0_k - w_k)
    w_k  <- 0 if its sign flipped (the sign belongs to the presynaptic neuron, Dale's law)
    w_k  <- clipped to +-WEIGHT_MAX_FACTOR * |w0_k|
    each learning row is rescaled so sum |w| equals its original sum (synaptic scaling),
    then clipped to the cap again (so the cap is exact and the row sum is at most the original)
    turn reference <- reference + REST_RATE * (steering output - reference)

Why each piece (all measured in simulation before choosing, see NOTES.md):
  * the trace: a kill comes 0.5 to 2 s after the turn that caused it (about 1 s at 30 fps);
  * reward minus its running mean: with a positive mean reward the covariance rule collapses
    a steering output to zero (seen in simulation);
  * the slow pull back to the connectome (FORGET_RATE, about 3 minutes) and the 2x cap bound
    how far learning can move the weights;
  * synaptic scaling: the model is stable because every row of |W| sums to at most 0.9
    (rate_model.py); keeping each learning row's sum at or below its original keeps that
    guarantee. Scaling can push a weight a little past the cap (measured: up to 0.1 %), so
    the cap is applied once more after scaling;
  * the moving turn reference: learning shifts the steering neurons' resting activity, and a
    fixed reference then turns that shift into a permanent turn bias (seen in simulation:
    12 % spurious turning). Only used while learning; with learning off the decoder uses the
    fixed MODEL_REST_* constants exactly as before.
"""

from __future__ import annotations

import numpy as np

LEARN_RATE = (
    0.2  # per unit reward and unit trace, relative to the synapse's original size
)
TRACE_DECAY = 0.967  # per frame; exp(-1 / 30): about 1 s at 30 frames per second
BASELINE_RATE = 0.01  # per frame; about 3 s
REWARD_MEAN_RATE = 0.001  # per frame; about 30 s
FORGET_RATE = 0.0002  # per frame; about 3 minutes back to the connectome weights
WEIGHT_MAX_FACTOR = 2.0  # |w| <= 2 |w0|
REST_RATE = 1.0 / 600.0  # per frame; turn reference follows the outputs over about 20 s

_F32 = np.float32


class Plasticity:
    """Learning state over the rows ``rows`` of a weight matrix shared with a RateModel.

    ``rows`` lists the left steering neurons first, then the right ones; ``n_left`` says how
    many are left. ``rest_left`` / ``rest_right`` are the fixed resting outputs of the groups.
    """

    def __init__(
        self,
        weights: np.ndarray,
        rows: list[int],
        n_left: int,
        rest_left: float,
        rest_right: float,
    ) -> None:
        self.w = weights  # shared with the RateModel: updates are seen by the next step
        self.rows = [int(i) for i in rows]
        self.n_left = n_left
        self.rest0 = (_F32(rest_left), _F32(rest_right))
        self.w0 = weights.copy()
        self.cols = [
            np.nonzero(self.w0[i])[0] for i in self.rows
        ]  # ascending, like CSR
        self.abs0 = [_abs_sum(self.w0[i, c]) for i, c in zip(self.rows, self.cols)]
        self.reset_weights()

    @property
    def n_learnable(self) -> int:
        return int(sum(len(c) for c in self.cols))

    def reset_weights(self) -> None:
        """Back to the connectome weights; traces, baselines and references reset."""
        for i, c in zip(self.rows, self.cols):
            self.w[i, c] = self.w0[i, c]
        self.trace = [np.zeros(len(c), dtype=np.float32) for c in self.cols]
        self.base = np.array(
            [self.rest0[0 if n < self.n_left else 1] for n in range(len(self.rows))],
            dtype=np.float32,
        )
        self.reward_mean = _F32(0.0)
        self.ref_left, self.ref_right = self.rest0

    def decode(self, steer_left: float, steer_right: float) -> tuple[bool, bool]:
        """Turn decoding against the moving reference (same rule as decode_action)."""
        from game.controller import TURN_DEADZONE, TURN_SIGN

        diff = (_F32(steer_right) - self.ref_right) - (_F32(steer_left) - self.ref_left)
        diff = diff * _F32(TURN_SIGN)
        return bool(diff > _F32(TURN_DEADZONE)), bool(diff < -_F32(TURN_DEADZONE))

    def update(
        self, r: np.ndarray, steer_left: float, steer_right: float, reward: float
    ) -> None:
        """Apply one frame of the rule given the activities after the model step."""
        for n, (i, c) in enumerate(zip(self.rows, self.cols)):
            post = _F32(r[i]) - self.base[n]
            self.trace[n] = _F32(TRACE_DECAY) * self.trace[n] + r[c] * post
            self.base[n] = self.base[n] + _F32(BASELINE_RATE) * (
                _F32(r[i]) - self.base[n]
            )
        reward = _F32(reward)
        surprise = reward - self.reward_mean
        self.reward_mean = self.reward_mean + _F32(REWARD_MEAN_RATE) * surprise
        for n, (i, c) in enumerate(zip(self.rows, self.cols)):
            w0 = self.w0[i, c]
            mag0 = np.abs(w0)
            w = self.w[i, c]
            w = w + (_F32(LEARN_RATE) * surprise) * self.trace[n] * mag0
            w = w + _F32(FORGET_RATE) * (w0 - w)
            w = np.where(w * w0 < 0, _F32(0.0), w)
            limit = _F32(WEIGHT_MAX_FACTOR) * mag0
            w = np.minimum(np.maximum(w, -limit), limit).astype(np.float32)
            total = _abs_sum(w)
            if total > 0:
                w = (w * (self.abs0[n] / total)).astype(np.float32)
                w = np.minimum(np.maximum(w, -limit), limit).astype(np.float32)
            self.w[i, c] = w
        rate = _F32(REST_RATE)
        self.ref_left = self.ref_left + rate * (_F32(steer_left) - self.ref_left)
        self.ref_right = self.ref_right + rate * (_F32(steer_right) - self.ref_right)

    def drift(self) -> float:
        """Relative L2 distance of the learning weights from the connectome: 0 = unchanged."""
        num = _F32(0.0)
        den = _F32(0.0)
        for i, c in zip(self.rows, self.cols):
            for k in range(len(c)):
                d = self.w[i, c[k]] - self.w0[i, c[k]]
                num += d * d
                den += self.w0[i, c[k]] * self.w0[i, c[k]]
        return float(np.sqrt(num / den)) if den > 0 else 0.0


def _abs_sum(values: np.ndarray) -> np.float32:
    """Sequential float32 sum of |values| (same order and rounding as the C loop)."""
    total = _F32(0.0)
    for v in values:
        total += np.abs(v)
    return total
