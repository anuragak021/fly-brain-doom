# drosophila: a fly-connectome-inspired controller playing Doom

A small neural controller whose wiring is taken from the open Drosophila male CNS connectome
(MaleCNS, neuPrint `male-cns:v1.0`) plays ViZDoom. This is a connectome **inspired** controller,
not a simulation of a fly brain.

## Status

| Phase | State |
|---|---|
| A: extract subgraph (Python) | done |
| B: rate model + ViZDoom loop, Python backend | done |
| C: CSR export to C + host test vs Python | done |
| D: STM32 firmware, UART link, `--backend stm32`, timing, video | **not started** (needs the Nucleo) |

Nothing here has run on the STM32 yet. No STM32 timing, no demo video.

## What was built and measured

* Subgraph: **300 neurons, 299,502 synapses** in 7,020 neuron-to-neuron connections
  (each connection has >= 5 synapses; 6,707 have a non-zero signed weight, the rest come from
  octopamine neurons that are dropped). 10 outputs, 14 inputs, 276 hidden.
* Host Python loop (laptop CPU, headless, 2 game tics per decision): about 1,650 decisions/s,
  controller step about 0.14 ms. These are host numbers, not STM32 numbers.
* Defend-the-center, 5 episodes: 0, 2, 2, 0, 1 kills. Episodes are short (about 110 to 200
  decisions). No baseline was run, so this does not show the controller plays well.
* When an enemy was off-centre, the controller turned toward it in 40 of 42 frames, away in 0.
* C model vs Python reference: 60 golden frames agree within abs 1e-5 + rel 1e-4 (`ctest`).
* Weights as CSR in flash: about 41 KB.

## Simplifications (all of them)

1. Only 300 neurons, chosen by a ranked upstream walk from 10 descending neurons, not the whole brain.
2. Outputs: DNa01 and DNa02 (steering), MDN (backward, logged only), and **DNp36 as the forward
   drive. DNp36 is a name-matched stand-in for P9: no neuron is called P9 in this dataset, and the
   only match is DNp36's synonym `pIP9`. That it is the walking-literature P9 is NOT verified.**
3. Weight = synapse count (>= 5 per pair), row-normalised, scaled to spectral radius 0.9.
4. Sign by presynaptic neurotransmitter: acetylcholine +, GABA and glutamate - (glutamate is
   inhibitory in many but not all fly synapses), octopamine dropped.
5. Rate model (leaky, ReLU, Euler), not spiking.
6. A constant tonic drive (0.2) feeds every non-input neuron. It is not from the connectome.
   Without it the network is silent.
7. Game features: left/right mean luminance and the largest enemy's horizontal position taken
   from ViZDoom's labels buffer. That is game-engine ground truth, not vision.
8. Turn direction is read from the two steering outputs. The connectome's natural response is
   ipsilateral suppression; the readout sign was chosen (`TURN_SIGN = +1`) so the agent turns toward
   the enemy. That choice is mine, not a connectome fact.
9. **Firing is a heuristic**, not connectome driven: fire when an enemy is near screen centre.
10. Forward/backward activities are logged only; the scenario has no move buttons.

## Reproduce

```
python -m venv .venv && . .venv/bin/activate && pip install -r requirements.txt
export NEUPRINT_APPLICATION_CREDENTIALS=<your neuPrint token>   # never commit it
python connectome/extract.py            # needs the token; writes connectome/data/*.json
python -m model.export_c                # CSR C files + golden vectors
pytest                                  # Python tests
cmake -S tests -B build/tests && cmake --build build/tests && ctest --test-dir build/tests
python -m game.play --backend python --episodes 5
```

`connectome/placeholder.py` writes a random graph with the same schema, flagged `placeholder`. It
is for development only and is not connectome data.

## Roadmap

STM32 brain firmware with a framed UART protocol and DWT step timing (Phase D); Doom on an ESP32-S3
with OLED; COBS/CRC reuse from the secure-boot project; GPU simulation of a larger network;
fixed point + CMSIS-DSP; spiking variant; delivery through the secure bootloader. See `NOTES.md`.
