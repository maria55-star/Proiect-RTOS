#include "uart.h"
#include <stdint.h>

#define RCC_AHB1ENR   (*(volatile uint32_t *)0x40023830)
#define RCC_APB2ENR   (*(volatile uint32_t *)0x40023844)

#define GPIOA_BASE    0x40020000
#define GPIOA_MODER   (*(volatile uint32_t *)(GPIOA_BASE + 0x00))
#define GPIOA_AFRH    (*(volatile uint32_t *)(GPIOA_BASE + 0x24))

#define USART1_BASE   0x40011000
#define USART1_SR     (*(volatile uint32_t *)(USART1_BASE + 0x00))
#define USART1_DR     (*(volatile uint32_t *)(USART1_BASE + 0x04))
#define USART1_BRR    (*(volatile uint32_t *)(USART1_BASE + 0x08))
#define USART1_CR1    (*(volatile uint32_t *)(USART1_BASE + 0x0C))

#define USART_SR_TXE  (1 << 7)

void uart_init(void)
{
    // GPIOA clock
    RCC_AHB1ENR |= (1 << 0);

    // USART1 clock (APB2, bit 4)
    RCC_APB2ENR |= (1 << 4);

    // PA9 = TX, PA10 = RX -> Alternate Function
    GPIOA_MODER &= ~((3u << (9*2)) | (3u << (10*2)));
    GPIOA_MODER |=  ((2u << (9*2)) | (2u << (10*2)));

    // AF7 on PA9/PA10 (AFRH pins 8..15)
    GPIOA_AFRH &= ~((0xFu << ((9-8)*4)) | (0xFu << ((10-8)*4)));
    GPIOA_AFRH |=  ((0x7u << ((9-8)*4)) | (0x7u << ((10-8)*4)));

    // Baud (merge și “aprox” în QEMU)
    USART1_BRR = 417;

    // UE | TE | RE
    USART1_CR1 = (1 << 13) | (1 << 3) | (1 << 2);
}

void uart_putc(char c)
{
    while (!(USART1_SR & USART_SR_TXE)) {}
    USART1_DR = (uint32_t)c;
}

void uart_puts(const char *s)
{
    while (*s) {
        if (*s == '\n') uart_putc('\r');
        uart_putc(*s++);
    }
}

void uart_print_uint(uint32_t val)
{
    char buf[12];
    int i = 0;

    if (val == 0) { uart_putc('0'); return; }

    while (val > 0) {
        buf[i++] = '0' + (val % 10);
        val /= 10;
    }
    while (i > 0) uart_putc(buf[--i]);
}


void uart_print_hex(uint32_t val) {
    const char hex[] = "0123456789ABCDEF";
    uart_puts("0x");
    for (int i = 7; i >= 0; i--) {
        uart_putc(hex[(val >> (i * 4)) & 0xF]);
    }
}