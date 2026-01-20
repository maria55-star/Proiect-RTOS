#ifndef RTOS_H
#define RTOS_H

#include <stdint.h>
#include <stddef.h>
#include "rtos_config.h"

// ----------------------------------------------
// Hardware Definitions (registrii SCB pentru PendSV)
// ----------------------------------------------
#define SCB_ICSR   (*(volatile uint32_t *)0xE000ED04) // Interrupt Control and State Register
#define SCB_ICSR_PENDSVSET (1UL << 28)                // bit-ul pentru a declansa PendSV

// ----------------------------------------------
// Task States
// ----------------------------------------------
typedef enum {
    TASK_READY = 0,
    TASK_DELAYED,
    TASK_BLOCKED_SEM,
    TASK_BLOCKED_MUTEX,
    TASK_BLOCKED_QUEUE
} task_state_t;

typedef enum {
    RTOS_WAIT_PENDING = 0,
    RTOS_WAIT_OK,
    RTOS_WAIT_TIMEOUT
} rtos_wait_result_t;

// ----------------------------------------------
// Task Control Block
// ----------------------------------------------
typedef struct rtos_tcb{
    uint32_t *stack_ptr;

    uint32_t base_priority;     // prioritate fixa (nu se schimba)
    uint32_t eff_priority;      // prioritate efectiva (PI/ceiling), folosita de scheduler

    task_state_t state;

    uint32_t wake_tick;         // pentru delay / timeout management

    void *wait_obj;             // sem/mutex/queue
    rtos_wait_result_t wait_res;// PENDING/ OK / TIMEOUT

    struct rtos_tcb *next;      // pt ready/delay lists
} rtos_tcb_t;

// ----------------------------------------------
// Semafor Structure
// ----------------------------------------------
typedef struct {
    volatile uint32_t count;         // 0 sau 1 pentru semafor binar
    // Putem adăuga o listă de task-uri care așteaptă acest semafor anume
} rtos_sem_t;

// ----------------------------------------------
// Mutex Structure
// ----------------------------------------------
typedef struct {
    volatile uint32_t lock;      // 0 = liber, 1 = ocupat
    rtos_tcb_t *owner;           // Task-ul care deține mutex-ul
    uint32_t original_priority;  // Prioritatea reală a owner-ului (pentru restaurare)
} rtos_mutex_t;

// ----------------------------------------------
// Message Queue Structure
// ----------------------------------------------
typedef struct {
    uint32_t buffer[8]; 
    uint32_t head; //unde scrie
    uint32_t tail; //de unde citeste
    rtos_sem_t sem_free_slots;  // numara locurile libere Initial 8
    rtos_sem_t sem_available_msgs; // Initial 0
    rtos_mutex_t lock;
} rtos_queue_t;

typedef struct rtos_timer {
    uint32_t period_ticks;
    uint32_t remaining_ticks;
    void (*callback)(void);
    uint8_t active;
    struct rtos_timer *next;
} rtos_timer_t;

// ----------------------------------------------
// API
// ----------------------------------------------
void rtos_init();
void rtos_task_create(void (*task_fn)(void), uint32_t priority);
void rtos_scheduler_next(void);                       
void rtos_start();
void rtos_delay(uint32_t ticks);
void rtos_tick_handler();
uint32_t rtos_now();
void rtos_yield();   //forteaza switch ul
//semafor 
void rtos_sem_init(rtos_sem_t *sem, uint32_t initial_count);
void rtos_sem_wait(rtos_sem_t *sem);   // Functie blocanta 
int rtos_sem_wait_timeout(rtos_sem_t *sem, uint32_t timeout_ticks);
int rtos_mutex_lock_timeout(rtos_mutex_t *mutex, uint32_t timeout_ticks);
void rtos_sem_signal(rtos_sem_t *sem); // Elibereaza semaforul
//mutex
void rtos_mutex_init(rtos_mutex_t *mutex);
void rtos_mutex_lock(rtos_mutex_t *mutex);
void rtos_mutex_unlock(rtos_mutex_t *mutex);
//coada de mesaje
void rtos_queue_init(rtos_queue_t *q);
void rtos_queue_send(rtos_queue_t *q, uint32_t msg);
int rtos_queue_send_timeout(rtos_queue_t *q, uint32_t msg, uint32_t timeout_ticks);
int rtos_queue_receive_timeout(rtos_queue_t *q, uint32_t *out, uint32_t timeout_ticks);
uint32_t rtos_queue_receive(rtos_queue_t *q);
//
void rtos_timer_init(rtos_timer_t *timer, uint32_t period_ms, void (*callback)(void));
void rtos_timer_start(rtos_timer_t *timer);
void rtos_timer_stop(rtos_timer_t *timer);
// Statistici Determinism
uint32_t rtos_get_context_switch_cycles(void);
uint32_t rtos_get_max_context_switch_cycles(void);
uint32_t rtos_get_isr_latency_cycles(void);
uint32_t rtos_get_max_isr_latency_cycles(void);
#endif