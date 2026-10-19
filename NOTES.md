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

## Phase A status

Waiting for the neuPrint token. Placeholder graph generator comes first so Phase B is not blocked.
