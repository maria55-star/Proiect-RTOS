#include "uart.h"

// USART2 pentru STM32F4 (PA2=TX, PA3=RX)
#define USART2_BASE   0x40004400
#define USART2_SR     (*(volatile uint32_t *)(USART2_BASE + 0x00))
#define USART2_DR     (*(volatile uint32_t *)(USART2_BASE + 0x04))
#define USART2_BRR    (*(volatile uint32_t *)(USART2_BASE + 0x08))
#define USART2_CR1    (*(volatile uint32_t *)(USART2_BASE + 0x0C))

#define RCC_APB1ENR   (*(volatile uint32_t *)0x40023840)
#define RCC_AHB1ENR   (*(volatile uint32_t *)0x40023830)

#define GPIOA_BASE    0x40020000
#define GPIOA_MODER   (*(volatile uint32_t *)(GPIOA_BASE + 0x00))
#define GPIOA_AFRL    (*(volatile uint32_t *)(GPIOA_BASE + 0x20))

#define USART_SR_TXE  (1 << 7)
#define USART_CR1_UE  (1 << 13)
#define USART_CR1_TE  (1 << 3)

void uart_init(void) {
    RCC_AHB1ENR |= (1 << 0);  
    RCC_APB1ENR |= (1 << 17); 

    GPIOA_MODER &= ~(0x3 << 4);  
    GPIOA_MODER |= (0x2 << 4);   
    GPIOA_AFRL &= ~(0xF << 8);
    GPIOA_AFRL |= (0x7 << 8);    

    // QEMU ignoră adesea valoarea BRR, dar are nevoie de UE și TE activate corect
    USART2_BRR = 417; 
    USART2_CR1 = (1 << 13) | (1 << 3) | (1 << 2); // Adaugă și RE (bit 2) pentru stabilitate
}

void uart_putc(char c) {
    // Asteptam pana cand registrul de transmisie e gol
    while (!(USART2_SR & (1 << 7))); 
    USART2_DR = c;
}

void uart_puts(const char *s) {
    while (*s) {
        // În terminalele Linux/WSL, \n singur nu aduce cursorul la începutul rândului
        if (*s == '\n') {
            uart_putc('\r');
        }
        uart_putc(*s++);
    }
}

void uart_print_uint(uint32_t val) {
    char buf[12];
    int i = 0;
    
    if (val == 0) {
        uart_putc('0');
        return;
    }
    
    while (val > 0) {
        buf[i++] = '0' + (val % 10);
        val /= 10;
    }
    
    while (i > 0) {
        uart_putc(buf[--i]);
    }
}

void uart_print_hex(uint32_t val) {
    const char hex[] = "0123456789ABCDEF";
    uart_puts("0x");
    for (int i = 7; i >= 0; i--) {
        uart_putc(hex[(val >> (i * 4)) & 0xF]);
    }
}