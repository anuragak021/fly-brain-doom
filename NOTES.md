# NOTES: design decisions and simplifications

Plain-language log of what was done, why, and the tradeoffs. Updated as work proceeds.

## Honesty statement

This is a connectome **inspired** controller, not a simulation of a fly brain.
Every simplification is listed in the "Simplifications" section below.

## Environment

- Python 3.10 venv in `.venv/` (created with `uv`), dependencies pinned in `requirements.txt`.
- `neuprint-python` reads the API token from `NEUPRINT_APPLICATION_CREDENTIALS`
  (checked in the library source). The token is never written to any file.
- ViZDoom 1.3.2 ships the standard scenarios (`basic.cfg`, `defend_the_center.cfg`).

## Conventions (fixed now so Python and C cannot disagree)

- **Weight matrix layout: `W[post, pre]`.** The model computes `W @ r`, so row `i` is
  everything flowing INTO neuron `i`. In CSR, one row = one postsynaptic neuron.
  A transpose bug would pass a C-vs-Python test (both sides wrong the same way),
  so this is stated in the export header too.
- **Dale's law:** the sign belongs to the presynaptic neuron, so it is applied per
  column of `W`.

## Simplifications

(Filled in as each is made. Nothing here yet.)

## Phase A status (clock started 2026-10-19 21:15 IST, 1.5 h box)

Findings so far, from live queries against neuPrint:

- Server `neuprint.janelia.org`, dataset **`male-cns:v1.0`** (v0.9 also exists; v1.0 is newer).
- Useful columns: `somaSide`, `consensusNt`, `predictedNt`, `predictedNtConfidence`,
  `superclass`, `synonyms`, `inputRois`, `outputRois`.

| Literature name | Dataset type | Count | Notes |
|---|---|---|---|
| DNa01 | DNa01 | 2 (L, R) | bodyIds 10442 (L), 10760 (R) |
| DNa02 | DNa02 | 2 (L, R) | bodyIds 523769 (L), 10360 (R) |
| MDN | MDN | 4 (2 L, 2 R) | bodyIds 11288, 12348 (L), 10763, 11332 (R) |
| P9 | **not found** | 0 | no type or instance named P9. Regex hits (LoVP9, MeVP9, P6-8P9) are unrelated. The `synonyms` field has no P9 either. |

Open item: pick a forward-walking descending neuron to replace P9. Not guessing a
name; will be chosen from queried data and recorded here as a deviation.

## Placeholder graph

`connectome/placeholder.py` writes `connectome/data/placeholder_graph.json`, a random graph
with the real schema, flagged `placeholder: true`. It exists only so Phase B is not blocked.
It is NOT connectome data and nothing built on it may be described as connectome derived.
