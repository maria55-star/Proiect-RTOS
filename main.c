#include "rtos.h"
#include "uart.h"


// GPIO Register Definitions for STM32F4xx (GPIOA PA5)
#define RCC_AHB1ENR   (*(volatile uint32_t *)0x40023830)
#define GPIOA_MODER   (*(volatile uint32_t *)0x40020000)
#define GPIOA_ODR     (*(volatile uint32_t *)0x40020014)

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


extern volatile uint32_t g_pi_enabled;
// ----------------------------------------------
// Variabile pentru task-uri producer/consumer
// ----------------------------------------------
rtos_queue_t q_date;
rtos_mutex_t demo_mutex;
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
static void gpio_init(void)
{
    RCC_AHB1ENR |= (1u << 0); // GPIOAEN
    // PA5 output
    GPIOA_MODER &= ~(3u << (5*2));
    GPIOA_MODER |=  (1u << (5*2));
}

// ----------------------------------------------
// Task-uri Producer/Consumer
// ----------------------------------------------
void task_producator(void)
{
    uint32_t count = 100;

    while (1) {
        test_producer_runs++;

        int rc = rtos_queue_send_timeout(&q_date, count, 10);

        if (rc == 0) {
            msj_trimise++;
            uart_puts("[PROD] Sent: ");
            uart_print_uint(count);
            uart_puts(" @ tick=");
            uart_print_uint(rtos_now());
            uart_puts("\n");
            count++;
        } else {
            uart_puts("[PROD] TIMEOUT send @ tick=");
            uart_print_uint(rtos_now());
            uart_puts("\n");
        }

        rtos_delay(20);
    }
}


void task_consumator(void) {
    rtos_delay(2000);
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
        // Toggle LED (PA5)
        GPIOA_ODR ^= (1u << 5);
        blink_count++;

        if(blink_count % 10 == 0) {
            uart_puts("[BLINK] Count: ");
            uart_print_uint(blink_count);
            uart_puts(" @ tick=");
            uart_print_uint(rtos_now());
            uart_puts("\n");
        }

        // 10 Hz blink: toggle la 50ms => ON+OFF = 100ms
        rtos_delay(50);
    }
}


// ----------------------------------------------
// Task-uri pentru Priority Inversion (GPUI/PI demo)
// ----------------------------------------------

// LOW: ia mutex si il tine mult (CPU work), ca sa existe "owner"
void task_pi_low_owner(void) {
    while (1) {
        rtos_mutex_lock(&demo_mutex);

        uint32_t start = rtos_now();
        while ((rtos_now() - start) < 200) {   // ~200ms tine mutex
            __asm volatile("nop");
        }

        rtos_mutex_unlock(&demo_mutex);
        rtos_delay(300);
    }
}

// MED: "fura" CPU ca sa creeze inversiunea (nu se blocheaza pe mutex)
void task_pi_medium_hog(void) {
    rtos_delay(50);

    while (1) {
        uint32_t start = rtos_now();
        while ((rtos_now() - start) < 500) {
            __asm volatile("nop");
        }
        rtos_delay(1);
    }
}


// HIGH: vrea mutex -> asteapta; aici vezi efectul PI/ceiling (GPUI)
void task_pi_high_waiter(void) {
    rtos_delay(200);   // IMPORTANT: înainte de while(1)

    while (1) {
        uint32_t t0 = rtos_now();
        rtos_mutex_lock(&demo_mutex);
        uint32_t waited = rtos_now() - t0;

        uart_puts("[PI] HIGH got mutex, waited=");
        uart_print_uint(waited);
        uart_puts(" ms\n");

        rtos_mutex_unlock(&demo_mutex);
        rtos_delay(200);
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
    uart_puts("HELLO UART\n");
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
    rtos_mutex_init(&demo_mutex);

    // Initializare si pornire timere
    /*rtos_timer_init(&timer_1sec, 1000, timer_1sec_callback);
    rtos_timer_init(&timer_500ms, 500, timer_500ms_callback);
    rtos_timer_start(&timer_1sec);
    rtos_timer_start(&timer_500ms);*/

    uart_puts("Creating tasks...\n");

    // Creare task-uri (prioritate crescătoare)
    rtos_task_create(idle_task, 0);           // Prioritate minimă
    rtos_task_create(task_gpio_blink, 1);     // Prioritate joasă - blink task
    rtos_task_create(task_producator, 2);     // Prioritate medie
    rtos_task_create(task_consumator, 3);     // Prioritate medie-înaltă
    //rtos_task_create(task_rms_t2, 4);         // T2 = 20ms → prioritate mare
    //rtos_task_create(task_rms_t1, 5);         // T1 = 5ms → prioritate maximă

    //rtos_task_create(task_pi_low_owner, 1);
    //rtos_task_create(task_pi_medium_hog, 3);
    //rtos_task_create(task_pi_high_waiter, 5);

    uart_puts("Starting scheduler...\n");
    uart_puts("=============================\n\n");

    // --- Priority inversion demo tasks (HIGH > MED > LOW)
   
    rtos_start();

    while(1){}
} 