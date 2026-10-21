/**
 * @file i_brain_input.h
 * @brief Turns the brain's replies into Doom key events, and counts the player's kills.
 *
 * Called from the engine task only. Keys are posted with D_PostEvent as key down / key up events
 * when the wanted state changes, so the engine sees ordinary keyboard input.
 */
#ifndef I_BRAIN_INPUT_H
#define I_BRAIN_INPUT_H

#include <stdint.h>

/** @brief Post key events for this tic from the latest brain reply (called from I_StartTic). */
void ib_poll(void);

/**
 * @brief Kills since boot, summed over lives and levels (the score shown on the HUD).
 *
 * The engine's own count restarts with each level; this one only grows.
 */
int32_t ib_session_kills(void);

#endif /* I_BRAIN_INPUT_H */
