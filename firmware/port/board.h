/**
 * @file board.h
 * @brief Nucleo-H753ZI board support: clock, SysTick millisecond tick, cycle counter, LEDs.
 */
#ifndef BOARD_H
#define BOARD_H

#include <stdbool.h>
#include <stdint.h>

/** User LEDs on the Nucleo-144 board. */
typedef enum { BOARD_LED_GREEN = 0, BOARD_LED_YELLOW = 1, BOARD_LED_RED = 2 } board_led_t;

/**
 * @brief Configure clocks, SysTick (1 ms) and the DWT cycle counter; enable GPIO clocks.
 *
 * Runs at 64 MHz (HSI) unless the firmware is built with BRAIN_CPU_400MHZ, in which case it
 * switches to 400 MHz from PLL1 and falls back to HSI if the PLL does not lock.
 */
void board_init(void);

/** @return Core clock in Hz as configured by board_init(). */
uint32_t board_core_hz(void);

/** @return Clock of the APB1 peripherals (USART2/3/4/5/7/8) in Hz. */
uint32_t board_pclk1_hz(void);

/** @return Clock of the APB2 peripherals (USART1/6) in Hz; equal to APB1 with the dividers used
 * here. */
uint32_t board_pclk2_hz(void);

/** @return Milliseconds since board_init(). Wraps after about 49 days. */
uint32_t board_millis(void);

/** @return Free-running core cycle counter (DWT->CYCCNT). */
uint32_t board_cycles(void);

/** Toggle one LED. */
void board_led_toggle(board_led_t led);

/** Set one LED on or off. */
void board_led_set(board_led_t led, bool on);

#endif /* BOARD_H */
