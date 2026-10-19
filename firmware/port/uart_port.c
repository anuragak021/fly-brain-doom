/**
 * @file uart_port.c
 * @brief USART3 (ST-Link VCP) and the ESP32 link driver (USART1 on PB6/PB7 by default, USART6 on
 *        PG14/PG9 with BRAIN_ESP_USART6). See uart_port.h.
 */
#include "uart_port.h"

#include "stm32h7xx.h"

#define RX_RING_BYTES 256U /* power of two: indices wrap with a mask */
#define RX_RING_MASK (RX_RING_BYTES - 1U)
#define TX_WAIT_LOOPS 0x00FFFFFFU
#define VCP_TX_PIN 8U /* PD8 */
#define VCP_RX_PIN 9U /* PD9 */
#ifdef BRAIN_ESP_USART6
#define ESP_USART USART6
#define ESP_IRQ USART6_IRQn
#define ESP_GPIO GPIOG
#define ESP_GPIO_CLK RCC_AHB4ENR_GPIOGEN
#define ESP_TX_PIN 14U /* PG14 */
#define ESP_RX_PIN 9U  /* PG9 */
#else
#define ESP_USART USART1
#define ESP_IRQ USART1_IRQn
#define ESP_GPIO GPIOB
#define ESP_GPIO_CLK RCC_AHB4ENR_GPIOBEN
#define ESP_TX_PIN 6U /* PB6, Arduino D1 on the Nucleo-H753ZI */
#define ESP_RX_PIN 7U /* PB7, Arduino D0 */
#endif
#define UART_AF 7U /* USART1, USART3 and USART6 are all AF7 on the pins used here */
#define AFR_PINS_PER_REG 8U
#define MODER_AF 2U
#define PIN_MODE_BITS 2U
#define AFR_BITS 4U
#define AFR_HIGH_FIRST_PIN 8U
#define OSPEEDR_HIGH 2U
#define MIN_BRR 16U /* oversampling-by-16 needs BRR >= 16 */

_Static_assert((RX_RING_BYTES & (RX_RING_BYTES - 1U)) == 0U, "ring size must be a power of two");

/* One RX ring per port. Shared between the port's ISR (producer: writes head) and the main loop
 * (consumer: writes tail). One writer per index on a single core, so volatile plus a data
 * barrier is enough. */
typedef struct {
    USART_TypeDef *regs;
    uint8_t ring[RX_RING_BYTES];
    volatile uint32_t head;
    volatile uint32_t tail;
    volatile uint32_t drops;
} uart_dev_t;

static uart_dev_t g_dev[UART_COUNT] = {
    [UART_VCP] = {.regs = USART3},
    [UART_ESP] = {.regs = ESP_USART},
};

static void rx_isr(uart_dev_t *dev)
{
    USART_TypeDef *u = dev->regs;
    const uint32_t isr = u->ISR;
    if ((isr & (USART_ISR_ORE | USART_ISR_FE | USART_ISR_NE)) != 0U) {
        /* Without clearing ORE the receiver would stop for good. */
        u->ICR = USART_ICR_ORECF | USART_ICR_FECF | USART_ICR_NECF;
        dev->drops++;
    }
    while ((u->ISR & USART_ISR_RXNE_RXFNE) != 0U) {
        const uint8_t byte = (uint8_t)u->RDR; /* reading RDR clears RXNE */
        const uint32_t head = dev->head;
        if (((head + 1U) & RX_RING_MASK) == dev->tail) {
            dev->drops++; /* ring full: drop it, the protocol CRC catches the damage */
        } else {
            dev->ring[head] = byte;
            __DMB();
            dev->head = (head + 1U) & RX_RING_MASK;
        }
    }
}

void USART3_IRQHandler(void)
{
    rx_isr(&g_dev[UART_VCP]);
}

#ifdef BRAIN_ESP_USART6
void USART6_IRQHandler(void)
#else
void USART1_IRQHandler(void)
#endif
{
    rx_isr(&g_dev[UART_ESP]);
}

static void pin_to_af(GPIO_TypeDef *port, uint32_t pin)
{
    const uint32_t shift = pin * PIN_MODE_BITS;
    port->MODER = (port->MODER & ~(3U << shift)) | (MODER_AF << shift);
    port->OSPEEDR = (port->OSPEEDR & ~(3U << shift)) | (OSPEEDR_HIGH << shift);
    const uint32_t reg = pin / AFR_PINS_PER_REG;
    const uint32_t afr_shift = (pin % AFR_PINS_PER_REG) * AFR_BITS;
    port->AFR[reg] = (port->AFR[reg] & ~(0xFU << afr_shift)) | (UART_AF << afr_shift);
}

/* Clocks, pins and IRQ number for the chosen port. */
static IRQn_Type enable_clocks_and_pins(uart_id_t id)
{
    if (id == UART_VCP) {
        RCC->AHB4ENR |= RCC_AHB4ENR_GPIODEN;
        RCC->APB1LENR |= RCC_APB1LENR_USART3EN;
        (void)RCC->APB1LENR; /* read back so the clock runs before the first register access */
        pin_to_af(GPIOD, VCP_TX_PIN);
        pin_to_af(GPIOD, VCP_RX_PIN);
        return USART3_IRQn;
    }
    RCC->AHB4ENR |= ESP_GPIO_CLK;
#ifdef BRAIN_ESP_USART6
    RCC->APB2ENR |= RCC_APB2ENR_USART6EN;
#else
    RCC->APB2ENR |= RCC_APB2ENR_USART1EN;
#endif
    (void)RCC->APB2ENR;
    pin_to_af(ESP_GPIO, ESP_TX_PIN);
    pin_to_af(ESP_GPIO, ESP_RX_PIN);
    return ESP_IRQ;
}

bool uart_init(uart_id_t id, uint32_t baud, uint32_t pclk_hz)
{
    if ((uint32_t)id >= (uint32_t)UART_COUNT) {
        return false;
    }
    const uint32_t brr = (baud == 0U) ? 0U : ((pclk_hz + (baud / 2U)) / baud);
    if ((baud == 0U) || (brr < MIN_BRR)) {
        return false;
    }
    uart_dev_t *dev = &g_dev[id];
    dev->head = 0U;
    dev->tail = 0U;
    dev->drops = 0U;
    const IRQn_Type irq = enable_clocks_and_pins(id);
    USART_TypeDef *u = dev->regs;
    u->CR1 = 0U;
    u->BRR = brr;
    u->ICR = USART_ICR_ORECF | USART_ICR_FECF | USART_ICR_NECF;
    u->CR1 = USART_CR1_TE | USART_CR1_RE | USART_CR1_RXNEIE_RXFNEIE | USART_CR1_UE;
    NVIC_ClearPendingIRQ(irq);
    NVIC_EnableIRQ(irq);
    return true;
}

bool uart_read_byte(uart_id_t id, uint8_t *byte)
{
    if (((uint32_t)id >= (uint32_t)UART_COUNT) || (byte == NULL)) {
        return false;
    }
    uart_dev_t *dev = &g_dev[id];
    const uint32_t tail = dev->tail;
    if (tail == dev->head) {
        return false;
    }
    *byte = dev->ring[tail];
    __DMB();
    dev->tail = (tail + 1U) & RX_RING_MASK;
    return true;
}

bool uart_write(uart_id_t id, const uint8_t *data, size_t len)
{
    if (((uint32_t)id >= (uint32_t)UART_COUNT) || ((data == NULL) && (len != 0U))) {
        return false;
    }
    USART_TypeDef *u = g_dev[id].regs;
    for (size_t i = 0U; i < len; i++) {
        uint32_t guard = TX_WAIT_LOOPS;
        while (((u->ISR & USART_ISR_TXE_TXFNF) == 0U) && (guard > 0U)) {
            guard--;
        }
        if (guard == 0U) {
            return false;
        }
        u->TDR = data[i];
    }
    uint32_t guard = TX_WAIT_LOOPS;
    while (((u->ISR & USART_ISR_TC) == 0U) && (guard > 0U)) {
        guard--;
    }
    return guard != 0U;
}

uint32_t uart_drop_count(uart_id_t id)
{
    return ((uint32_t)id < (uint32_t)UART_COUNT) ? g_dev[id].drops : 0U;
}
