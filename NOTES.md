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

1. Neuron subset only: 300 neurons chosen by ranked upstream walk from 10 descending neurons (not the whole brain).
2. Forward drive uses DNp36 as a name-matched, unverified stand-in for P9.
3. Weight = raw synapse count (>= 5 per pair, summed over ROIs), then normalized. Synapse size and position ignored.
4. Signs by presynaptic neurotransmitter: acetylcholine +1; GABA and glutamate -1 (glutamate is
   inhibitory via GluCl in many fly circuits but excitatory at some synapses); octopamine and other
   modulators weight 0 (dropped). Neurons below 0.5 prediction confidence would be 'unknown' and dropped.
5. Rate model, not spiking.
6. Row normalisation (each neuron's total input magnitude = 1) then global scaling so the
   spectral radius of |W| = 0.9. This guarantees no runaway but also discards the absolute
   synapse-count differences between neurons.
7. A constant tonic drive (0.2) is given to every non-input neuron. It is NOT in the connectome.
   Without it the net is silent: the 14 visual inputs alone do not reach the outputs.
8. Features: left/right mean luminance (grey 160x120 buffer, left half vs right half) and the
   horizontal position of the largest enemy taken from ViZDoom's labels buffer. The enemy
   position is game-engine ground truth, not a vision pipeline. Enemy offset to the left drives
   the left input neurons, to the right the right ones (proportional to |offset|).
9. Turn decoding uses the steering outputs relative to their resting state:
   diff = (steerR - steerL) - (rest diff). Positive means turn left. **Sign choice:** measured
   response is that driving one side's visual inputs LOWERS that side's DNa activity
   (ipsilateral suppression). If DNa activity meant "turn toward that side", the agent would
   turn AWAY from enemies. `TURN_SIGN = +1` selects the opposite readout (turn toward the
   stimulus) so the agent approaches; this is a decoding choice made by me, not a connectome fact.
10. Fire is a HEURISTIC, not connectome driven: it fires when an enemy is within 0.15 of screen
    centre. Forward/backward outputs are only logged (defend_the_center has no move buttons).

## Phase B results (Python backend, defend_the_center, 5 episodes, CPU)

- Rate model: tau = 20 ms, Euler dt = 5 ms (alpha 0.25), 6 substeps per decision, float32.
- Measured response with tonic 0.2 and input gain 5: left visual drive changed steerL by -0.099 and
  steerR by +0.011; right drive changed steerR by -0.107 and steerL by -0.012 (mirror symmetric).
- When an enemy was off-centre the controller turned toward it in 40 of 42 frames and away in 0
  (the other 2 were inside the deadzone). Frames are few because episodes are short.
- Kills per episode: 0, 2, 2, 0, 1 (mean 1.0). No baseline comparison yet, so this is not
  evidence the controller plays well.
- Python controller step: about 0.14 ms; whole loop about 1650 decisions/s (headless, 2 tics per decision).
- 10 host tests pass (`pytest`): features, weight convention, stability bound, left/right turning.

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

**P9 deviation:** no neuron is named P9. The only P9-like match in the `synonyms` column is
`DNp36` (`Yu 2010: pIP9`, `Cachero 2010: pIP-h`), bodyIds 10245 (R), 10712 (L), both predicted
glutamate. I use DNp36 as the "forward" output. Whether it is the P9 of the walking literature
is NOT verified, so it is a name-matched stand-in and the forward drive is labelled as such.

### Extraction result (`connectome/extract.py`, run 2026-10-19)

- 300 neurons, 7020 edges (synapse count >= 5 summed over ROIs per pair; total synapses not yet tallied).
- 10 outputs (DNa01 x2, DNa02 x2, MDN x4, DNp36 x2), 14 inputs, 276 hidden.
- Inputs are visual projection neurons reached by the walk (LT51, LC33, LoVC4, MeVPLp1),
  9 left and 5 right, so the "visual superclass" rule worked and no fallback was needed.
- Hop 1: top 100 upstream partners of the outputs by synapse count. Hop 2: top 190 by
  synapses into hop 1. The size cap is enforced by ranking.
- NT counts: 142 acetylcholine, 81 GABA, 75 glutamate, 2 octopamine; none unknown.
- Edge signs: 3408 excitatory, 3299 inhibitory, 313 zero (octopamine neurons, dropped).
- The heaviest edge was cross-checked against `fetch_simple_connections` (totals match).
- A bare `Client(...)` that is not kept in a variable is garbage collected and the default
  client vanishes; the script holds a reference.

## Placeholder graph

`connectome/placeholder.py` writes `connectome/data/placeholder_graph.json`, a random graph
with the real schema, flagged `placeholder: true`. It exists only so Phase B is not blocked.
It is NOT connectome data and nothing built on it may be described as connectome derived.
