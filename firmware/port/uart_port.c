/**
 * @file uart_port.c
 * @brief USART3 (ST-Link VCP) driver. See uart_port.h.
 */
#include "uart_port.h"

#include "stm32h7xx.h"

#define RX_RING_BYTES 256U /* power of two: indices wrap with a mask */
#define RX_RING_MASK (RX_RING_BYTES - 1U)
#define TX_WAIT_LOOPS 0x00FFFFFFU
#define VCP_TX_PIN 8U /* PD8 */
#define VCP_RX_PIN 9U /* PD9 */
#define VCP_AF 7U     /* USART3 */
#define MODER_AF 2U
#define PIN_MODE_BITS 2U
#define AFR_BITS 4U
#define AFR_HIGH_FIRST_PIN 8U
#define OSPEEDR_HIGH 2U
#define MIN_BRR 16U /* oversampling-by-16 needs BRR >= 16 */

_Static_assert((RX_RING_BYTES & (RX_RING_BYTES - 1U)) == 0U, "ring size must be a power of two");

/* Shared between the USART3 ISR (producer: writes head) and the main loop (consumer: writes
 * tail). One writer per index on a single core, so volatile plus a data barrier is enough. */
static uint8_t g_ring[RX_RING_BYTES];
static volatile uint32_t g_head;
static volatile uint32_t g_tail;
static volatile uint32_t g_drops;

void USART3_IRQHandler(void)
{
    const uint32_t isr = USART3->ISR;
    if ((isr & (USART_ISR_ORE | USART_ISR_FE | USART_ISR_NE)) != 0U) {
        /* Without clearing ORE the receiver would stop for good. */
        USART3->ICR = USART_ICR_ORECF | USART_ICR_FECF | USART_ICR_NECF;
        g_drops++;
    }
    while ((USART3->ISR & USART_ISR_RXNE_RXFNE) != 0U) {
        const uint8_t byte = (uint8_t)USART3->RDR; /* reading RDR clears RXNE */
        const uint32_t head = g_head;
        if (((head + 1U) & RX_RING_MASK) == g_tail) {
            g_drops++; /* ring full: drop it, the protocol CRC catches the damage */
        } else {
            g_ring[head] = byte;
            __DMB();
            g_head = (head + 1U) & RX_RING_MASK;
        }
    }
}

static void pin_to_af(uint32_t pin)
{
    const uint32_t shift = pin * PIN_MODE_BITS;
    GPIOD->MODER = (GPIOD->MODER & ~(3U << shift)) | (MODER_AF << shift);
    GPIOD->OSPEEDR = (GPIOD->OSPEEDR & ~(3U << shift)) | (OSPEEDR_HIGH << shift);
    const uint32_t afr_shift = (pin - AFR_HIGH_FIRST_PIN) * AFR_BITS;
    GPIOD->AFR[1] = (GPIOD->AFR[1] & ~(0xFU << afr_shift)) | (VCP_AF << afr_shift);
}

bool uart_init(uint32_t baud, uint32_t pclk_hz)
{
    const uint32_t brr = (pclk_hz + (baud / 2U)) / baud;
    if ((baud == 0U) || (brr < MIN_BRR)) {
        return false;
    }
    g_head = 0U;
    g_tail = 0U;
    g_drops = 0U;
    RCC->AHB4ENR |= RCC_AHB4ENR_GPIODEN;
    RCC->APB1LENR |= RCC_APB1LENR_USART3EN;
    (void)RCC->APB1LENR; /* read back so the clock is running before the first register access */
    pin_to_af(VCP_TX_PIN);
    pin_to_af(VCP_RX_PIN);
    USART3->CR1 = 0U;
    USART3->BRR = brr;
    USART3->ICR = USART_ICR_ORECF | USART_ICR_FECF | USART_ICR_NECF;
    USART3->CR1 = USART_CR1_TE | USART_CR1_RE | USART_CR1_RXNEIE_RXFNEIE | USART_CR1_UE;
    NVIC_ClearPendingIRQ(USART3_IRQn);
    NVIC_EnableIRQ(USART3_IRQn);
    return true;
}

bool uart_read_byte(uint8_t *byte)
{
    const uint32_t tail = g_tail;
    if ((byte == NULL) || (tail == g_head)) {
        return false;
    }
    *byte = g_ring[tail];
    __DMB();
    g_tail = (tail + 1U) & RX_RING_MASK;
    return true;
}

bool uart_write(const uint8_t *data, size_t len)
{
    if ((data == NULL) && (len != 0U)) {
        return false;
    }
    for (size_t i = 0U; i < len; i++) {
        uint32_t guard = TX_WAIT_LOOPS;
        while (((USART3->ISR & USART_ISR_TXE_TXFNF) == 0U) && (guard > 0U)) {
            guard--;
        }
        if (guard == 0U) {
            return false;
        }
        USART3->TDR = data[i];
    }
    uint32_t guard = TX_WAIT_LOOPS;
    while (((USART3->ISR & USART_ISR_TC) == 0U) && (guard > 0U)) {
        guard--;
    }
    return guard != 0U;
}

uint32_t uart_drop_count(void)
{
    return g_drops;
}
