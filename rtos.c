#include "rtos.h"

// DWT (Data Watchpoint and Trace) pentru măsurare cicluri
#define DWT_CTRL     (*(volatile uint32_t *)0xE0001000)
#define DWT_CYCCNT   (*(volatile uint32_t *)0xE0001004)
#define DEM_CR       (*(volatile uint32_t *)0xE000EDFC)

#define DEM_CR_TRCENA (1 << 24)
#define DWT_CTRL_CYCCNTENA (1 << 0)

#define SCB_SHPR3 (*(volatile uint32_t *)0xE000ED20) 
// Adresa SCB_ICSR (pentru PendSV trigger)
#define SCB_ICSR  (*(volatile uint32_t *)0xE000ED04)
#define SCB_ICSR_PENDSVSET (1UL << 28)
// ----------------------------------------------
// Pool static de TCB-uri si stive
// ----------------------------------------------
static rtos_tcb_t tcb_pool[RTOS_MAX_TASKS];
static uint32_t tcb_count = 0;
static uint32_t task_stacks[RTOS_MAX_TASKS][RTOS_STACK_SIZE] __attribute__((aligned(8)));
static rtos_tcb_t *current_task = NULL;
static rtos_tcb_t *ready_lists[RTOS_MAX_PRIORITIES];
static rtos_tcb_t *delay_list = NULL; // lista sortata dupa wake_tick (simplu, max task-uri mici)
static uint32_t top_priority_mask = 0;
volatile uint32_t g_tick = 0;
static volatile uint32_t rtos_started=0;
extern void systick_init(void);
//Lista de timere și statistici determinism
static rtos_timer_t *timer_list = NULL;

static volatile uint32_t context_switch_cycles = 0;
static volatile uint32_t max_context_switch_cycles = 0;
volatile uint32_t isr_latency_cycles = 0;
volatile uint32_t max_isr_latency_cycles = 0;
static volatile uint32_t last_cs_cycles = 0;
static volatile uint32_t max_cs_cycles = 0;
// forward declarations
static void ready_insert(rtos_tcb_t *t);
static void ready_remove(rtos_tcb_t *t);
static void task_set_eff_priority(rtos_tcb_t *t, uint32_t new_eff);
static uint32_t get_next_task_priority(uint32_t mask);
static void set_exception_priorities(void);
static void dwt_init(void);

// ----------------------------------------------
// Functii pentru Tick
// ----------------------------------------------
void rtos_tick_handler()
{
    g_tick++;

    // 1) wake delayed tasks
    while (delay_list && (int32_t)(g_tick - delay_list->wake_tick) >= 0) {
        rtos_tcb_t *t = delay_list;
        delay_list = delay_list->next;

        t->state = TASK_READY;
        t->next = NULL;
        ready_insert(t);
    }

    // 2) timeouts pentru task-uri blocate (scan pool - max mic, ok)
    for (uint32_t i = 0; i < tcb_count; i++) {
        rtos_tcb_t *t = &tcb_pool[i];
        if ((t->state == TASK_BLOCKED_SEM ||
             t->state == TASK_BLOCKED_MUTEX ||
             t->state == TASK_BLOCKED_QUEUE) &&
             t->wake_tick != 0 &&
            t->wait_res == RTOS_WAIT_PENDING)
        {
            if ((int32_t)(g_tick - t->wake_tick) >= 0) {
                // timeout expirat
                t->state = TASK_READY;
                t->wait_obj = NULL;
                t->wait_res = RTOS_WAIT_TIMEOUT;
                t->wake_tick = 0;
                ready_insert(t);
            }
        }
    }

    // 3) soft timers (lasam, dar vezi nota de ISR minimal)
    // (optional: lasam callback-urile sa fie super scurte)
    rtos_timer_t *timer = timer_list;
    while (timer != NULL) {
        if (timer->active) {
            timer->remaining_ticks--;
            if (timer->remaining_ticks == 0) {
                timer->remaining_ticks = timer->period_ticks;
                if (timer->callback) timer->callback();
            }
        }
        timer = timer->next;
    }

    // Declansam PendSV pentru a verifica dacă un task proaspat trezit are prioritate mai mare
    SCB_ICSR = SCB_ICSR_PENDSVSET;
}

uint32_t rtos_now(){
    return g_tick;
}

static void ready_insert(rtos_tcb_t *t)
{
    uint32_t p = t->eff_priority;

    if (ready_lists[p] == NULL) {
        t->next = t;
        ready_lists[p] = t;
        top_priority_mask |= (1u << p);
        return;
    }

    t->next = ready_lists[p]->next;
    ready_lists[p]->next = t;
}

static void ready_remove(rtos_tcb_t *t)
{
    uint32_t p = t->eff_priority;
    rtos_tcb_t *head = ready_lists[p];
    if (!head) return;

    // lista circulara: cautam predecesorul
    rtos_tcb_t *prev = head;
    while (prev->next != t && prev->next != head) {
        prev = prev->next;
    }
    if (prev->next != t) return; // nu e in lista

    if (t == head) {
        if (head->next == head) {
            ready_lists[p] = NULL;
            top_priority_mask &= ~(1u << p);
        } else {
            ready_lists[p] = head->next;
            prev->next = head->next;
        }
    } else {
        prev->next = t->next;
    }

    t->next = NULL;
}
 
static void task_set_eff_priority(rtos_tcb_t *t, uint32_t new_eff)
{
    if (t->eff_priority == new_eff) return;

    // daca e READY, trebuie mutat intre ready_lists
    if (t->state == TASK_READY) {
        ready_remove(t);
        t->eff_priority = new_eff;
        ready_insert(t);
    } else {
        t->eff_priority = new_eff;
    }
}

static uint32_t get_next_task_priority(uint32_t mask)
{
    if(mask==0) return 0;
    //calcul O(1) pentru gasirea celei mai inalte prioritati cu __buitlin_clz
    return 31 - __builtin_clz(mask);
}

static void set_exception_priorities()
{
    // Setează PendSV la 255 (cea mai mică) și SysTick la ceva mai mare (ex. 128)
    SCB_SHPR3 = (0xFF << 16) | (0x80 << 24);
}

// Functie pentru initializare DWT
static void dwt_init(void) {
    // Enable trace
    DEM_CR |= DEM_CR_TRCENA;
    
    // Reset counter
    DWT_CYCCNT = 0;
    
    // Enable counter
    DWT_CTRL |= DWT_CTRL_CYCCNTENA;
}

// ----------------------------------------------
// Initializare RTOS
// ----------------------------------------------
void rtos_init(){
    tcb_count = 0;
    current_task = NULL;
    delay_list = NULL;
    top_priority_mask = 0;
    for (uint32_t i = 0; i < RTOS_MAX_PRIORITIES; i++) ready_lists[i] = NULL;

    set_exception_priorities();
    dwt_init();
}

// ----------------------------------------------
// selecteaza urmatorul task de rulat
// ----------------------------------------------
void rtos_scheduler_next() {
    if(tcb_count == 0) return;

    uint32_t temp_mask = top_priority_mask;
    while(temp_mask > 0) {
        // Găsim cea mai înaltă prioritate din masca temporară
        uint32_t prio =get_next_task_priority(temp_mask);
        rtos_tcb_t *start_task = ready_lists[prio];
        rtos_tcb_t *search = start_task;

        do {
            // Task-ul este eligibil DOAR daca este in starea READY
            if (search->state == TASK_READY) {
                current_task = search;
                ready_lists[prio] = search;
                return;
}

            search = search->next;
        } while (search != start_task);

        // Dacă nu am găsit nimic la prioritatea asta, o eliminăm din mască și căutăm la următoarea
        temp_mask &= ~(1u << prio);
    }
}
// ----------------------------------------------
// PendSV_Handler pentru context switching
// ----------------------------------------------
__attribute__((naked))
void PendSV_Handler(void)
{
    __asm volatile(
        "MRS   r0, PSP                \n"
        "CBZ   r0, 1f                 \n"
        "STMDB r0!, {r4-r11}          \n"
        "LDR   r1, =current_task      \n"
        "LDR   r2, [r1]               \n"
        "STR   r0, [r2]               \n"  // current_task->stack_ptr = PSP
        "1:                           \n"

        "PUSH  {lr}                   \n"
        "BL    rtos_scheduler_next    \n"
        "POP   {lr}                   \n"

        "LDR   r1, =current_task      \n"
        "LDR   r2, [r1]               \n"
        "LDR   r0, [r2]               \n"  // r0 = next_task->stack_ptr
        "LDMIA r0!, {r4-r11}          \n"
        "MSR   PSP, r0                \n"

        // IMPORTANT: intoarcere in Thread mode folosind PSP
        "LDR   lr, =0xFFFFFFFD        \n"
        "BX    lr                     \n"
    );
}


// ----------------------------------------------
// Creare task
// ----------------------------------------------
void rtos_task_create(void (*task_fn)(void), uint32_t priority){
    if(tcb_count >= RTOS_MAX_TASKS){
        return;
    }

    rtos_tcb_t *tcb = &tcb_pool[tcb_count];
    tcb->base_priority = priority;
    tcb->eff_priority  = priority;
    tcb->state = TASK_READY;
    tcb->wait_obj = NULL;
    tcb->wait_res = RTOS_WAIT_OK;
    tcb->wake_tick = 0;

    uint32_t *stack = task_stacks[tcb_count];
    uint32_t size = RTOS_STACK_SIZE;

    stack[size - 1] = 0x01000000;           // xPSR (thumb bit = 1)
    stack[size - 2] = (uint32_t)task_fn | 0x01;  // PC = functia task-ului
    stack[size - 3] = 0xFFFFFFFD;           // LR pentru thread mode cu PSP (stiva separata)
    stack[size - 4] = 0;                    // R12
    stack[size - 5] = 0;                    // R3
    stack[size - 6] = 0;                    // R2
    stack[size - 7] = 0;                    // R1
    stack[size - 8] = 0;                    // R0

    // context software (R4..R11) va fi salvat/restaurat ulterior
    tcb->stack_ptr = &stack[size - 16];

    ready_insert(tcb);

    tcb_count++;
}
// ----------------------------------------------
// Pornire scheduler
// ----------------------------------------------
void rtos_start(){
    rtos_scheduler_next(); // Alege primul task
    
    __asm volatile("mov r0, #0 \n msr psp, r0"); // Spune-i lui PendSV că e prima rulare
    
    set_exception_priorities();
    systick_init();
    
    __asm volatile("cpsie i" : : : "memory"); 

    SCB_ICSR = SCB_ICSR_PENDSVSET;
    while (1) { /* nimic */ }
    //for(;;) { __asm volatile("wfi"); }

}

// ----------------------------------------------
// Contect switch prin PendSV
// ----------------------------------------------
void rtos_yield() 
{
    SCB_ICSR = SCB_ICSR_PENDSVSET; //declansare PendSV
}
void rtos_delay(uint32_t ticks)
{
    if (ticks == 0) return;

    __asm volatile("cpsid i" : : : "memory");

    current_task->state = TASK_DELAYED;
    current_task->wake_tick = g_tick + ticks;

    // scoate din READY
    ready_remove(current_task);

    // insereaza sortat in delay_list (lista simpla)
    rtos_tcb_t **pp = &delay_list;
    while (*pp && (int32_t)((*pp)->wake_tick - current_task->wake_tick) <= 0) {
        pp = &(*pp)->next;
    }
    current_task->next = *pp;
    *pp = current_task;

    __asm volatile("cpsie i" : : : "memory");

    rtos_yield();
}

// ----------------------------------------------
// Semafor binar
// ----------------------------------------------
void rtos_sem_init(rtos_sem_t *sem, uint32_t initial_count) {
    sem->count = initial_count; //0 sau 1 pt sem binar
}
void rtos_sem_wait(rtos_sem_t *sem)
{
    (void)rtos_sem_wait_timeout(sem, 0xFFFFFFFFu);
}


int rtos_sem_wait_timeout(rtos_sem_t *sem, uint32_t timeout_ticks)
{
    while (1) {
        __asm volatile("cpsid i" : : : "memory");

        // 1) semafor disponibil -> il luam si iesim
        if (sem->count > 0) {
            sem->count--;
            current_task->wait_res = RTOS_WAIT_OK;      // <-- CORECT
            current_task->wake_tick = 0;
            current_task->wait_obj = NULL;
            __asm volatile("cpsie i" : : : "memory");
            return 0;
        }

        // 2) timeout imediat
        if (timeout_ticks == 0) {
            current_task->wait_res = RTOS_WAIT_TIMEOUT;
            __asm volatile("cpsie i" : : : "memory");
            return 1;
        }

        // 3) blocam task-ul pe semafor
        current_task->state = TASK_BLOCKED_SEM;
        current_task->wait_obj = (void*)sem;
        current_task->wait_res = RTOS_WAIT_PENDING;    // <-- CORECT

        // set timeout: wake_tick=0 inseamna "infinit"
        if (timeout_ticks != 0xFFFFFFFFu) {
            current_task->wake_tick = g_tick + timeout_ticks;
        } else {
            current_task->wake_tick = 0;
        }

        // scoate din ready list (ca sa nu mai fie ales)
        ready_remove(current_task);

        __asm volatile("cpsie i" : : : "memory");

        // lasa scheduler-ul sa ruleze alt task
        rtos_yield();

        // 4) cand revine aici, ori a fost semnalat, ori a expirat timeout-ul
        if (current_task->wait_res == RTOS_WAIT_TIMEOUT) {
            return 1;
        }

        // daca a fost deblocat (OK), reluam bucla si incercam sa luam semaforul atomic
        // (asta evita race-condition: signal intre wake si decrement)
    }
}


void rtos_sem_signal(rtos_sem_t *sem)
{
    __asm volatile("cpsid i" : : : "memory");

    sem->count++;

    // trezim un singur task care asteapta semaforul
    for (uint32_t i = 0; i < tcb_count; i++) {
        if (tcb_pool[i].state == TASK_BLOCKED_SEM && tcb_pool[i].wait_obj == sem) {
            tcb_pool[i].state = TASK_READY;
            tcb_pool[i].wait_obj = NULL;
            tcb_pool[i].wait_res = RTOS_WAIT_OK;
            tcb_pool[i].wake_tick = 0;
            ready_insert(&tcb_pool[i]);
            break;
        }
    }

    __asm volatile("cpsie i" : : : "memory");
    rtos_yield(); // verificam daca task-ul deblocat are prioritate mai mare
}

// ----------------------------------------------
// Mutex cu Priority Inheritance
// ----------------------------------------------
void rtos_mutex_init(rtos_mutex_t *mutex) {
    if (mutex == NULL) return;

    mutex->lock = 0;                // Mutex-ul este liber inițial
    mutex->owner = NULL;            // Nu aparține niciunui task
    mutex->original_priority = 0;   // Valoare neutră
}

void rtos_mutex_lock(rtos_mutex_t *mutex)
{
    (void)rtos_mutex_lock_timeout(mutex, 0xFFFFFFFFu);
}

int rtos_mutex_lock_timeout(rtos_mutex_t *mutex, uint32_t timeout_ticks)
{
    while (1) {
        __asm volatile("cpsid i" : : : "memory");

        if (mutex->lock == 0) {
            mutex->lock = 1;
            mutex->owner = current_task;
            mutex->original_priority = current_task->base_priority; // baza, nu eff
            current_task->wait_res = RTOS_WAIT_OK;
            __asm volatile("cpsie i" : : : "memory");
            return 0;
        }

        // PI: daca eu sunt mai sus, ridic owner-ul EFECTIV
        if (mutex->owner && current_task->eff_priority > mutex->owner->eff_priority) {
            task_set_eff_priority(mutex->owner, current_task->eff_priority);
        }

        // blocam pe mutex
        current_task->state = TASK_BLOCKED_MUTEX;
        current_task->wait_obj = mutex;
        current_task->wait_res = RTOS_WAIT_PENDING;

        if (timeout_ticks == 0) {
            current_task->state = TASK_READY;
            current_task->wait_obj = NULL;
            current_task->wait_res = RTOS_WAIT_TIMEOUT;
            __asm volatile("cpsie i" : : : "memory");
            return 1;
        } else if (timeout_ticks != 0xFFFFFFFFu) {
            current_task->wake_tick = g_tick + timeout_ticks;
        } else {
            current_task->wake_tick = 0;
        }

        ready_remove(current_task);

        __asm volatile("cpsie i" : : : "memory");
        rtos_yield();

        if (current_task->wait_res == RTOS_WAIT_TIMEOUT) return 1;
    }
}

void rtos_mutex_unlock(rtos_mutex_t *mutex)
{
    __asm volatile("cpsid i" : : : "memory");

    if (mutex->owner != current_task) {
        __asm volatile("cpsie i" : : : "memory");
        return;
    }

    // restore owner eff prio la base
    task_set_eff_priority(current_task, current_task->base_priority);

    mutex->lock = 0;
    mutex->owner = NULL;

    // trezim primul waiter
    for (uint32_t i = 0; i < tcb_count; i++) {
        rtos_tcb_t *t = &tcb_pool[i];
        if (t->state == TASK_BLOCKED_MUTEX && t->wait_obj == mutex) {
            t->state = TASK_READY;
            t->wait_obj = NULL;
            t->wait_res = RTOS_WAIT_OK;
            t->wake_tick = 0;
            ready_insert(t);
            break;
        }
    }

    __asm volatile("cpsie i" : : : "memory");
    rtos_yield();
}

// ----------------------------------------------
// Message Queue
// ----------------------------------------------
void rtos_queue_init(rtos_queue_t *q) {
    q->head = 0;
    q->tail = 0;
    rtos_sem_init(&q->sem_free_slots, 8);
    rtos_sem_init(&q->sem_available_msgs, 0);
    rtos_mutex_init(&q->lock);
}
void rtos_queue_send(rtos_queue_t *q, uint32_t msg)
{
    (void)rtos_queue_send_timeout(q, msg, 0xFFFFFFFFu);
}


int rtos_queue_send_timeout(rtos_queue_t *q, uint32_t msg, uint32_t timeout_ticks)
{
    if (rtos_sem_wait_timeout(&q->sem_free_slots, timeout_ticks) != 0) return 1;

    rtos_mutex_lock(&q->lock);
    q->buffer[q->head] = msg;
    q->head = (q->head + 1) % 8;
    rtos_mutex_unlock(&q->lock);

    rtos_sem_signal(&q->sem_available_msgs);
    return 0;
}

uint32_t rtos_queue_receive(rtos_queue_t *q)
{
    uint32_t v;
    (void)rtos_queue_receive_timeout(q, &v, 0xFFFFFFFFu);
    return v;
}

int rtos_queue_receive_timeout(rtos_queue_t *q, uint32_t *out, uint32_t timeout_ticks)
{
    if (rtos_sem_wait_timeout(&q->sem_available_msgs, timeout_ticks) != 0) return 1;

    rtos_mutex_lock(&q->lock);
    *out = q->buffer[q->tail];
    q->tail = (q->tail + 1) % 8;
    rtos_mutex_unlock(&q->lock);

    rtos_sem_signal(&q->sem_free_slots);
    return 0;
}

//Implementare Soft Timers
static uint32_t ms_to_ticks(uint32_t ms)
{
    // rotunjire in sus: (ms * tick_rate + 999) / 1000 la tick_rate=1000
    uint64_t t = (uint64_t)ms * (uint64_t)RTOS_TICK_RATE_HZ;
    return (uint32_t)((t + 999) / 1000);
}

void rtos_timer_init(rtos_timer_t *timer, uint32_t period_ms, void (*callback)(void))
{
    timer->period_ticks = ms_to_ticks(period_ms);
    timer->remaining_ticks = timer->period_ticks;
    timer->callback = callback;
    timer->active = 0;
    timer->next = NULL;
}


void rtos_timer_start(rtos_timer_t *timer) {
    __asm volatile("cpsid i" : : : "memory");
    
    timer->active = 1;
    timer->remaining_ticks = timer->period_ticks;
    
    if (timer_list == NULL) {
        timer_list = timer;
        timer->next = NULL;
    } else {
        timer->next = timer_list;
        timer_list = timer;
    }
    
    __asm volatile("cpsie i" : : : "memory");
}

void rtos_timer_stop(rtos_timer_t *timer) {
    __asm volatile("cpsid i" : : : "memory");
    timer->active = 0;
    __asm volatile("cpsie i" : : : "memory");
}

// Funcții pentru accesare statistici determinism
uint32_t rtos_get_context_switch_cycles(void) { 
    return last_cs_cycles; 
}
uint32_t rtos_get_max_context_switch_cycles(void) { 
    return max_cs_cycles; 
}

uint32_t rtos_get_isr_latency_cycles(void) {
    // Returnează diferența în tick-uri (ar trebui 1 mereu)
    return isr_latency_cycles;
}

uint32_t rtos_get_max_isr_latency_cycles(void) {
    return max_isr_latency_cycles;
}