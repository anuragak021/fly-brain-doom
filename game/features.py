"""Turn a ViZDoom frame into the small feature vector sent to the controller.

Pure numpy / plain data, no ViZDoom import, so it is unit testable on the host.

Feature vector (float32, all in 0..1), order fixed by the FEATURE_* indices:
    [luminance_left, luminance_right, enemy_left, enemy_right, damage_left, damage_right]

Simplifications (listed in NOTES.md):
  * luminance is the mean of the grey screen buffer, left half vs right half;
  * the enemy position comes from ViZDoom's labels buffer (game-engine ground
    truth, NOT a vision pipeline); the largest visible monster is used;
  * damage_left / damage_right (which side the last attacker is on, fading as the pain wears
    off) are computed only by the ESP32 Doom port; ViZDoom frames leave them at 0.
"""

from __future__ import annotations

from collections.abc import Iterable
from typing import NamedTuple

import numpy as np

FEATURE_LUM_L = 0
FEATURE_LUM_R = 1
FEATURE_ENEMY_L = 2
FEATURE_ENEMY_R = 3
FEATURE_DAMAGE_L = 4
FEATURE_DAMAGE_R = 5
N_FEATURES = 6
GRAY_MAX = 255.0

# Labels that count as enemies (ViZDoom object names); everything else is ignored.
ENEMY_NAMES = frozenset(
    {
        "Zombieman",
        "ShotgunGuy",
        "ChaingunGuy",
        "DoomImp",
        "Demon",
        "Spectre",
        "LostSoul",
        "Cacodemon",
        "BaronOfHell",
        "HellKnight",
        "Revenant",
        "Arachnotron",
        "Archvile",
        "Fatso",
        "PainElemental",
        "WolfensteinSS",
    }
)


class Label(NamedTuple):
    """Minimal view of a ViZDoom label: object name and its bounding box in pixels."""

    name: str
    x: float
    y: float
    width: float
    height: float


def luminance_halves(gray: np.ndarray) -> tuple[float, float]:
    """Mean brightness (0..1) of the left and right halves of a grey image (H, W)."""
    if gray.ndim != 2 or gray.shape[1] < 2:
        raise ValueError(f"expected a 2-D grey image with width >= 2, got {gray.shape}")
    half = gray.shape[1] // 2
    left = float(gray[:, :half].mean()) / GRAY_MAX
    right = float(gray[:, gray.shape[1] - half :].mean()) / GRAY_MAX
    return left, right


def enemy_offset(labels: Iterable[Label], frame_width: int) -> float | None:
    """Horizontal position of the largest enemy in -1 (left edge) .. +1 (right edge).

    Returns None if no enemy is visible.
    """
    best: Label | None = None
    for lab in labels:
        if lab.name in ENEMY_NAMES and (
            best is None or lab.width * lab.height > best.width * best.height
        ):
            best = lab
    if best is None:
        return None
    centre = best.x + best.width / 2.0
    return float(np.clip(2.0 * centre / frame_width - 1.0, -1.0, 1.0))


def make_features(
    gray: np.ndarray, labels: Iterable[Label]
) -> tuple[np.ndarray, float | None]:
    """Build the feature vector and also return the enemy offset (None if no enemy)."""
    lum_l, lum_r = luminance_halves(gray)
    offset = enemy_offset(labels, gray.shape[1])
    feat = np.zeros(N_FEATURES, dtype=np.float32)
    feat[FEATURE_LUM_L] = lum_l
    feat[FEATURE_LUM_R] = lum_r
    if offset is not None:
        feat[FEATURE_ENEMY_L] = max(-offset, 0.0)
        feat[FEATURE_ENEMY_R] = max(offset, 0.0)
    return feat, offset
