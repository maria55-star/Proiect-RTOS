#include "rtos.h"
#include "uart.h"

// ADAUGĂ ASTA:
#define USART2_SR     (*(volatile uint32_t *)0x40004400)
#define USART2_DR     (*(volatile uint32_t *)0x40004404)

// GPIO Register Definitions for STM32F1xx
#define RCC_APB2ENR  (*(volatile uint32_t *)0x40021018)
#define GPIOC_CRL    (*(volatile uint32_t *)0x40011000)
#define GPIOC_CRH    (*(volatile uint32_t *)0x40011004)
#define GPIOC_ODR    (*(volatile uint32_t *)0x4001100C)
#define GPIOC_IDR    (*(volatile uint32_t *)0x40011008)

// RCC APB2ENR bits
#define RCC_APB2ENR_IOPCEN (1u << 4)

// GPIO Configuration bits
#define GPIO_CRL_MODE0_SHIFT 0
#define GPIO_CRL_MODE0_MASK  (3u << GPIO_CRL_MODE0_SHIFT)
#define GPIO_CRL_CNF0_SHIFT  2
#define GPIO_CRL_CNF0_MASK   (3u << GPIO_CRL_CNF0_SHIFT)

// GPIO Mode values
#define GPIO_MODE_INPUT  0x0
#define GPIO_MODE_OUTPUT_10MHZ 0x1
#define GPIO_MODE_OUTPUT_2MHZ  0x2
#define GPIO_MODE_OUTPUT_50MHZ 0x3

// GPIO CNF values for output
#define GPIO_CNF_OUTPUT_PUSHPULL  0x0
#define GPIO_CNF_OUTPUT_OPENDRAIN 0x1
#define GPIO_CNF_OUTPUT_AF_PUSHPULL 0x2
#define GPIO_CNF_OUTPUT_AF_OPENDRAIN 0x3

extern uint32_t rtos_now();

// ----------------------------------------------
// SysTick
// ----------------------------------------------
#define SYST_CSR   (*(volatile uint32_t *)0xE000E010)
#define SYST_RVR   (*(volatile uint32_t *)0xE000E014)
#define SYST_CVR   (*(volatile uint32_t *)0xE000E018)
#define SCB_VTOR   (*(volatile uint32_t *)0xE000ED08)

#define SYST_CSR_ENABLE      (1u << 0)
#define SYST_CSR_TICKINT     (1u << 1)
#define SYST_CSR_CLKSOURCE   (1u << 2)

// ----------------------------------------------
// Variabile pentru task-uri producer/consumer
// ----------------------------------------------
rtos_queue_t q_date;
volatile uint32_t msj_trimise = 0;
volatile uint32_t msj_primite = 0;
volatile uint32_t ultimul_mesaj = 0;

// VARIABILE PENTRU TEST
volatile uint32_t test_idle_runs = 0;
volatile uint32_t test_producer_runs = 0;
volatile uint32_t test_consumer_runs = 0;

// ----------------------------------------------
// Test soft timers
// ----------------------------------------------
rtos_timer_t timer_1sec;
rtos_timer_t timer_500ms;
volatile uint32_t timer_1sec_ticks = 0;
volatile uint32_t timer_500ms_ticks = 0;

void timer_1sec_callback(void) {
    timer_1sec_ticks++;
}

void timer_500ms_callback(void) {
    timer_500ms_ticks++;
}

// ----------------------------------------------
// Variabile pentru RMS Demo
// ----------------------------------------------
volatile uint32_t t1_executions = 0;
volatile uint32_t t1_deadline_misses = 0;
volatile uint32_t t2_executions = 0;
volatile uint32_t t2_deadline_misses = 0;

void systick_init()
{
    uint32_t reload = (CPU_CLOCK_HZ / RTOS_TICK_RATE_HZ) - 1u;
    if(reload > 0x00FFFFFFu){
        while(1){}
    }

    SYST_RVR = reload;
    SYST_CVR = 0u;
    SYST_CSR = SYST_CSR_ENABLE | SYST_CSR_TICKINT | SYST_CSR_CLKSOURCE;
}

// ----------------------------------------------
// GPIO Initialization
// ----------------------------------------------
void gpio_init(void) {
    // Enable GPIOC clock
    RCC_APB2ENR |= RCC_APB2ENR_IOPCEN;
    
    // Configure PC13 as push-pull output (LED on many STM32 boards)
    // Clear MODE13 and CNF13 bits first
    GPIOC_CRH &= ~((3u << 20) | (3u << 22));
    // Set MODE13 = 11 (50MHz output) and CNF13 = 00 (push-pull)
    GPIOC_CRH |= (3u << 20) | (0u << 22);
    
    // Initialize LED off (PC13 is active low on many boards)
    GPIOC_ODR |= (1u << 13);
}

// ----------------------------------------------
// Task-uri Producer/Consumer
// ----------------------------------------------
void task_producator(void) {
    uint32_t count = 100;
    while(1) {
        test_producer_runs++;
        rtos_delay(1000);
        rtos_queue_send(&q_date, count);
        msj_trimise++;

        uart_puts("[PROD] Sent: ");
        uart_print_uint(count);
        uart_puts(" @ tick=");
        uart_print_uint(rtos_now());
        uart_puts("\n");

        count++;
    }
}

void task_consumator(void) {
    while(1) {
        test_consumer_runs++;
        ultimul_mesaj = rtos_queue_receive(&q_date);
        msj_primite++;

        uart_puts("[CONS] Received: ");
        uart_print_uint(ultimul_mesaj);
        uart_puts(" @ tick=");
        uart_print_uint(rtos_now());
        uart_puts("\n");
    }
}

// ----------------------------------------------
// Task-uri RMS
// ----------------------------------------------
// T1: perioadă 5ms, execuție ~1ms, prioritate MARE
void task_rms_t1(void) {
    uint32_t next_release = rtos_now() + 5;
    uint32_t count = 0;

    while(1) {
        uint32_t start = rtos_now();
        
        // Simulare execuție 1ms
        while(rtos_now() - start < 1);
        
        t1_executions++;
        
        // Verifică deadline
        if(rtos_now() > next_release) {
            t1_deadline_misses++;
            uart_puts("[T1] DEADLINE MISS!\n");
        }
        
         if(++count >= 100) {
            uart_puts("[T1] Exec=");
            uart_print_uint(t1_executions);
            uart_puts(" Misses=");
            uart_print_uint(t1_deadline_misses);
            uart_puts("\n");
            count = 0;
        }

        uint32_t now = rtos_now();
        if(now < next_release) {
            rtos_delay(next_release - now);
        }
        next_release += 5;
    }
}

// T2: perioadă 20ms, execuție ~2ms, prioritate MEDIE
void task_rms_t2(void) {
    uint32_t next_release = rtos_now() + 20;
    uint32_t count = 0;

    while(1) {
        uint32_t start = rtos_now();
        
        // Simulare execuție 2ms
        while(rtos_now() - start < 2);
        
        t2_executions++;
        
        // Verifică deadline
        if(rtos_now() > next_release) {
            t2_deadline_misses++;
            uart_puts("[T2] DEADLINE MISS!\n");
        }
        
        if(++count >= 25) {
            uart_puts("[T2] Exec=");
            uart_print_uint(t2_executions);
            uart_puts(" Misses=");
            uart_print_uint(t2_deadline_misses);
            uart_puts("\n");
            count = 0;
        }

        uint32_t now = rtos_now();
        if(now < next_release) {
            rtos_delay(next_release - now);
        }
        next_release += 20;
    }
}

// ----------------------------------------------
// GPIO Blink Task
// ----------------------------------------------
volatile uint32_t blink_count = 0;

void task_gpio_blink(void) {
    while(1) {
        // Toggle LED (PC13)
        GPIOC_ODR ^= (1u << 13);
        blink_count++;
        
        // Print blink status every 10 blinks
        if(blink_count % 10 == 0) {
            uart_puts("[BLINK] Count: ");
            uart_print_uint(blink_count);
            uart_puts(" @ tick=");
            uart_print_uint(rtos_now());
            uart_puts("\n");
        }
        
        // Delay for 500ms (adjust for desired blink rate)
        rtos_delay(500);
    }
}

// ----------------------------------------------
// Idle Task
// ----------------------------------------------
void idle_task(void) {
    uint32_t last_tick = 0;
    while(1) {
        test_idle_runs++;
        
        uint32_t now = rtos_now();
        if(now >= 10000 && last_tick < 10000) {
            // După 10 secunde, verifică toate valorile:
            // msj_trimise ~ 10
            // msj_primite ~ 10
            // timer_1sec_ticks ~ 10
            // timer_500ms_ticks ~ 20
            // t1_executions ~ 2000
            // t2_executions ~ 500
            // t1_deadline_misses = 0
            // t2_deadline_misses = 0
            while(1) {
                __asm volatile("nop");
            }
        }
        last_tick = now;
    }
}

// ----------------------------------------------
// Main
// ----------------------------------------------
int main(){
    // Dezactivează temporar întreruperile la începutul main-ului pentru siguranță
    __asm volatile("cpsid i");
    SCB_VTOR = 0x08000000;

    uart_init();
    gpio_init();  

    /*uart_putc('A'); // Dacă vezi 'A', UART-ul hardware e OK
    uart_putc('\n');*/

    
    uart_puts("\n=============================\r\n");
    uart_puts("    RTOS Boot Sequence\n");
    uart_puts("=============================\r\n");
    uart_puts("Tick rate: ");
    uart_print_uint(RTOS_TICK_RATE_HZ);
    uart_puts(" Hz\n");
    uart_puts("Max tasks: ");
    uart_print_uint(RTOS_MAX_TASKS);
    uart_puts("\n");

    rtos_init();
    rtos_queue_init(&q_date);

    // Initializare si pornire timere
    rtos_timer_init(&timer_1sec, 1000, timer_1sec_callback);
    rtos_timer_init(&timer_500ms, 500, timer_500ms_callback);
    rtos_timer_start(&timer_1sec);
    rtos_timer_start(&timer_500ms);

    uart_puts("Creating tasks...\n");

    // Creare task-uri (prioritate crescătoare)
    rtos_task_create(idle_task, 0);           // Prioritate minimă
    rtos_task_create(task_gpio_blink, 1);     // Prioritate joasă - blink task
    rtos_task_create(task_producator, 2);     // Prioritate medie
    rtos_task_create(task_consumator, 3);     // Prioritate medie-înaltă
    rtos_task_create(task_rms_t2, 4);         // T2 = 20ms → prioritate mare
    rtos_task_create(task_rms_t1, 5);         // T1 = 5ms → prioritate maximă

    uart_puts("Starting scheduler...\n");
    uart_puts("=============================\n\n");

    rtos_start();

    while(1){}
} 