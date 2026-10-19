/**
 * @file board.c
 * @brief Nucleo-H753ZI board support. Register level on CMSIS, no HAL.
 *
 * Clock tree for the optional 400 MHz build (RM0433): HSI 64 MHz / DIVM1 16 = 4 MHz PLL input,
 * x DIVN1 200 = 800 MHz VCO (wide range, 192..836 MHz), / DIVP1 2 = 400 MHz SYSCLK.
 * D1CPRE /1 (400 MHz core), HPRE /2 (200 MHz AXI/AHB), every APB /2 (100 MHz).
 * Needs voltage scale 1 and 2 flash wait states at 200 MHz AXI.
 */
#include "board.h"

#include "stm32h7xx.h"

#define HSI_HZ 64000000U
#define MS_PER_S 1000U
#define PLL_WAIT_LOOPS 1000000U
#define VOS_WAIT_LOOPS 1000000U

#define LED_GREEN_PIN 0U  /* PB0 */
#define LED_YELLOW_PIN 1U /* PE1 */
#define LED_RED_PIN 14U   /* PB14 */
#define MODER_OUTPUT 1U
#define PIN_MODE_BITS 2U
#define BSRR_RESET_SHIFT 16U

#define DWT_UNLOCK_KEY 0xC5ACCE55U

#ifdef BRAIN_CPU_400MHZ
#define PLL_DIVM1 16U
#define PLL_DIVN1 200U
#define PLL_DIVP1 2U
#define PLL_INPUT_RANGE_2_4MHZ 1U /* PLL1RGE encoding for a 2..4 MHz reference */
#define FLASH_WAIT_STATES 2U
#define FLASH_WRHIGHFREQ 2U
#define CORE_HZ 400000000U
#define PCLK1_HZ 100000000U
#else
#define CORE_HZ HSI_HZ
#define PCLK1_HZ HSI_HZ
#endif

/* Written by the SysTick ISR, read by the main loop. volatile: shared with an ISR. */
static volatile uint32_t g_ms;
static uint32_t g_core_hz = HSI_HZ;
static uint32_t g_pclk1_hz = HSI_HZ;

void SysTick_Handler(void)
{
    g_ms++;
}

#ifdef BRAIN_CPU_400MHZ
/** Switch to 400 MHz from PLL1. Returns false (and stays on HSI) if a wait times out. */
static bool clock_to_400mhz(void)
{
    PWR->D3CR = (PWR->D3CR & ~PWR_D3CR_VOS) | PWR_D3CR_VOS; /* VOS1 = 0b11 */
    uint32_t guard = VOS_WAIT_LOOPS;
    while (((PWR->D3CR & PWR_D3CR_VOSRDY) == 0U) && (guard > 0U)) {
        guard--;
    }
    if (guard == 0U) {
        return false;
    }
    FLASH->ACR = (FLASH->ACR & ~(FLASH_ACR_LATENCY | FLASH_ACR_WRHIGHFREQ)) |
                 (FLASH_WAIT_STATES << FLASH_ACR_LATENCY_Pos) |
                 (FLASH_WRHIGHFREQ << FLASH_ACR_WRHIGHFREQ_Pos);

    RCC->PLLCKSELR = (RCC->PLLCKSELR & ~(RCC_PLLCKSELR_PLLSRC | RCC_PLLCKSELR_DIVM1)) |
                     RCC_PLLCKSELR_PLLSRC_HSI | (PLL_DIVM1 << RCC_PLLCKSELR_DIVM1_Pos);
    RCC->PLLCFGR = (RCC->PLLCFGR & ~(RCC_PLLCFGR_PLL1RGE | RCC_PLLCFGR_PLL1VCOSEL)) |
                   (PLL_INPUT_RANGE_2_4MHZ << RCC_PLLCFGR_PLL1RGE_Pos) | RCC_PLLCFGR_DIVP1EN;
    RCC->PLL1DIVR =
        ((PLL_DIVN1 - 1U) << RCC_PLL1DIVR_N1_Pos) | ((PLL_DIVP1 - 1U) << RCC_PLL1DIVR_P1_Pos);
    RCC->CR |= RCC_CR_PLL1ON;
    guard = PLL_WAIT_LOOPS;
    while (((RCC->CR & RCC_CR_PLL1RDY) == 0U) && (guard > 0U)) {
        guard--;
    }
    if (guard == 0U) {
        RCC->CR &= ~RCC_CR_PLL1ON;
        return false;
    }
    RCC->D1CFGR = RCC_D1CFGR_HPRE_DIV2 | RCC_D1CFGR_D1PPRE_DIV2; /* D1CPRE /1 */
    RCC->D2CFGR = RCC_D2CFGR_D2PPRE1_DIV2 | RCC_D2CFGR_D2PPRE2_DIV2;
    RCC->D3CFGR = RCC_D3CFGR_D3PPRE_DIV2;
    RCC->CFGR = (RCC->CFGR & ~RCC_CFGR_SW) | RCC_CFGR_SW_PLL1;
    guard = PLL_WAIT_LOOPS;
    while (((RCC->CFGR & RCC_CFGR_SWS) != RCC_CFGR_SWS_PLL1) && (guard > 0U)) {
        guard--;
    }
    if (guard == 0U) {
        RCC->CFGR = (RCC->CFGR & ~RCC_CFGR_SW) | RCC_CFGR_SW_HSI;
        RCC->D1CFGR = 0U;
        RCC->D2CFGR = 0U;
        RCC->D3CFGR = 0U;
        return false;
    }
    return true;
}
#endif

static void gpio_output(GPIO_TypeDef *port, uint32_t pin)
{
    const uint32_t shift = pin * PIN_MODE_BITS;
    port->MODER = (port->MODER & ~(3U << shift)) | (MODER_OUTPUT << shift);
}

static void dwt_enable(void)
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->LAR = DWT_UNLOCK_KEY;
    DWT->CYCCNT = 0U;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

void board_init(void)
{
#ifdef BRAIN_CPU_400MHZ
    if (clock_to_400mhz()) {
        g_core_hz = CORE_HZ;
        g_pclk1_hz = PCLK1_HZ;
    }
#endif
#ifdef BRAIN_CACHES
    SCB_EnableICache();
    SCB_EnableDCache();
#endif
    RCC->AHB4ENR |= RCC_AHB4ENR_GPIOBEN | RCC_AHB4ENR_GPIODEN | RCC_AHB4ENR_GPIOEEN;
    (void)RCC->AHB4ENR; /* read back: let the clock enable take effect before touching GPIO */
    gpio_output(GPIOB, LED_GREEN_PIN);
    gpio_output(GPIOE, LED_YELLOW_PIN);
    gpio_output(GPIOB, LED_RED_PIN);
    dwt_enable();
    (void)SysTick_Config(g_core_hz / MS_PER_S);
}

uint32_t board_core_hz(void)
{
    return g_core_hz;
}

uint32_t board_pclk1_hz(void)
{
    return g_pclk1_hz;
}

uint32_t board_millis(void)
{
    return g_ms;
}

uint32_t board_cycles(void)
{
    return DWT->CYCCNT;
}

static GPIO_TypeDef *led_port(board_led_t led)
{
    return (led == BOARD_LED_YELLOW) ? GPIOE : GPIOB;
}

static uint32_t led_pin(board_led_t led)
{
    switch (led) {
    case BOARD_LED_GREEN:
        return LED_GREEN_PIN;
    case BOARD_LED_YELLOW:
        return LED_YELLOW_PIN;
    default:
        return LED_RED_PIN;
    }
}

void board_led_set(board_led_t led, bool on)
{
    const uint32_t bit = 1U << led_pin(led);
    led_port(led)->BSRR = on ? bit : (bit << BSRR_RESET_SHIFT);
}

void board_led_toggle(board_led_t led)
{
    const uint32_t bit = 1U << led_pin(led);
    GPIO_TypeDef *port = led_port(led);
    port->BSRR = ((port->ODR & bit) != 0U) ? (bit << BSRR_RESET_SHIFT) : bit;
}
