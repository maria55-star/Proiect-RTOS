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
// ----------------------------------------------
// Functii pentru Tick
// ----------------------------------------------
void rtos_tick_handler(){
    g_tick++;
    // Parcurgem toate task-urile din pool-ul static
    for (uint32_t i = 0; i < tcb_count; i++) {
        if (tcb_pool[i].delay_ticks > 0) {
            tcb_pool[i].delay_ticks--;
        }
    }
    //SOFT TIMERS - proceseaza în tail-ul ISR-ului
    rtos_timer_t *timer = timer_list;
    while (timer != NULL) {
        if (timer->active) {
            timer->remaining_ticks--;
            if (timer->remaining_ticks == 0) {
                timer->remaining_ticks = timer->period_ticks; // Reload
                if (timer->callback != NULL) {
                    timer->callback(); // Apelează callback-ul
                }
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
            // Task-ul este eligibil DOAR dacă nu are delay ȘI este în starea READY
            if (search->delay_ticks == 0 && search->state==TASK_READY) {
                current_task = search;
                ready_lists[prio] = search; // Rotim lista pentru Round Robin
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
        "MRS r0, PSP\n"               
        "CBZ r0, skip_save\n"           
        "STMDB r0!, {r4-r11}\n"       
        "LDR r1, =current_task\n"
        "LDR r1, [r1]\n"
        "STR r0, [r1]\n"
        
        "skip_save:\n"
        
        // Incrementează counter pentru context switch
        "LDR r2, =context_switch_cycles\n"
        "LDR r1, [r2]\n"
        "ADDS r1, r1, #1\n"
        "STR r1, [r2]\n"
        
        // Actualizează max
        "LDR r2, =max_context_switch_cycles\n"
        "LDR r0, [r2]\n"
        "CMP r1, r0\n"
        "IT GT\n"
        "STRGT r1, [r2]\n"
        
        "BL rtos_scheduler_next\n"
        
        "LDR r1, =current_task\n"
        "LDR r1, [r1]\n"
        "LDR r0, [r1]\n"             
        "LDMIA r0!, {r4-r11}\n"       
        "MSR PSP, r0\n"
        
        "LDR r0, =0xFFFFFFFD\n" 
        "MOV lr, r0\n"
        "BX lr\n"
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
    tcb->priority = priority;
    tcb->delay_ticks = 0;
    tcb->state = TASK_READY;
    tcb->wait_obj = NULL;

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

    //priority list management
    if (ready_lists[priority] == NULL) {
        tcb->next = tcb; // Primul task punctează la el însuși
        ready_lists[priority] = tcb;
    } else {
        // Inserăm task-ul nou în lista circulară existentă
        tcb->next = ready_lists[priority]->next;
        ready_lists[priority]->next = tcb;
    }
    
    top_priority_mask |= (1u << priority);
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

    SCB_ICSR = SCB_ICSR_PENDSVSET; // Declanșează PendSV
    while(1); 
}

// ----------------------------------------------
// Contect switch prin PendSV
// ----------------------------------------------
void rtos_yield() 
{
    SCB_ICSR = SCB_ICSR_PENDSVSET; //declansare PendSV
}
void rtos_delay(uint32_t ticks) {
    if (ticks == 0) return;

    __asm volatile("cpsid i" : : : "memory");
    current_task->delay_ticks = ticks;
    __asm volatile("cpsie i" : : : "memory");

    rtos_yield(); // Forțăm un context switch pentru a lăsa alt task să ruleze
}
// ----------------------------------------------
// Semafor binar
// ----------------------------------------------
void rtos_sem_init(rtos_sem_t *sem, uint32_t initial_count) {
    sem->count = initial_count; //0 sau 1 pt sem binar
}
void rtos_sem_wait(rtos_sem_t *sem) {
    while(1){
        __asm volatile("cpsid i" : : : "memory"); //logica de blocare/deblocare
        if (sem->count > 0) {
            sem->count--;
            __asm volatile("cpsie i" : : : "memory");
            return; // Am obținut semnalul, ieșim
        } else {
            // Blocăm task-ul 
            current_task->state = TASK_BLOCKED_SEM;
            current_task->wait_obj = (void*)sem;
            __asm volatile("cpsie i" : : : "memory");
            rtos_yield(); // Forțăm switch-ul către un alt task
        }
    }
}
void rtos_sem_signal(rtos_sem_t *sem) {
    __asm volatile("cpsid i" : : : "memory");

    sem->count++;

    // Căutăm în pool un task care aștepta acest semafor specific
    for (uint32_t i = 0; i < tcb_count; i++) {
        if (tcb_pool[i].state == TASK_BLOCKED_SEM && tcb_pool[i].wait_obj == sem) {
            tcb_pool[i].state = TASK_READY;
            tcb_pool[i].wait_obj = NULL;
            break; // Deblocăm doar primul task (FIFO-ish simplificat)
        }
    }

    __asm volatile("cpsie i" : : : "memory");
    rtos_yield(); //verificăm dacă task-ul deblocat are prioritate mai mare
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
void rtos_mutex_lock(rtos_mutex_t *mutex) {
    while (1) {
        __asm volatile("cpsid i" : : : "memory");

        if (mutex->lock == 0) {
            // Mutex liber - îl ocupăm
            mutex->lock = 1;
            mutex->owner = current_task;
            mutex->original_priority = current_task->priority;
            __asm volatile("cpsie i" : : : "memory");
            return;
        } else {
            // Mutex ocupat - verificăm Inversiunea de Prioritate
            if (current_task->priority > mutex->owner->priority) {
                // Ridicăm prioritatea deținătorului (Inheritance)
                mutex->owner->priority = current_task->priority;
                
                // Actualizăm masca de priorități a scheduler-ului
                top_priority_mask |= (1u << mutex->owner->priority);
            }

            current_task->state = TASK_BLOCKED_MUTEX;
            current_task->wait_obj = (void*)mutex;
            __asm volatile("cpsie i" : : : "memory");
            rtos_yield();
        }
    }
}
void rtos_mutex_unlock(rtos_mutex_t *mutex) {
    __asm volatile("cpsid i" : : : "memory");

    if (mutex->owner != current_task) {
        __asm volatile("cpsie i" : : : "memory");
        return; // Doar proprietarul poate debloca
    }

    // Restaurăm prioritatea originală dacă a fost moștenită
    if (current_task->priority != mutex->original_priority) {
        // Curățăm bitul de prioritate înaltă din mască (dacă nu mai sunt alte task-uri acolo)
        // Notă: O implementare completă ar verifica dacă mai există task-uri la acea prioritate, 
        // dar pentru simplitate, restaurăm valoarea.
        current_task->priority = mutex->original_priority;
    }

    mutex->lock = 0;
    mutex->owner = NULL;

    // Trezim task-urile care așteptau acest mutex
    for (uint32_t i = 0; i < tcb_count; i++) {
        if (tcb_pool[i].state == TASK_BLOCKED_MUTEX && tcb_pool[i].wait_obj == mutex) {
            tcb_pool[i].state = TASK_READY;
            tcb_pool[i].wait_obj = NULL;
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
void rtos_queue_send(rtos_queue_t *q, uint32_t msg) {
    rtos_sem_wait(&q->sem_free_slots); // Așteaptă loc liber
    rtos_mutex_lock(&q->lock);         // Protejează buffer-ul
    
    q->buffer[q->head] = msg;
    q->head = (q->head + 1) % 8;
    
    rtos_mutex_unlock(&q->lock);
    rtos_sem_signal(&q->sem_available_msgs); // Anunță că există mesaj
}
uint32_t rtos_queue_receive(rtos_queue_t *q) {
    uint32_t msg;
    rtos_sem_wait(&q->sem_available_msgs); // Așteaptă mesaj
    rtos_mutex_lock(&q->lock);
    
    msg = q->buffer[q->tail];
    q->tail = (q->tail + 1) % 8;
    
    rtos_mutex_unlock(&q->lock);
    rtos_sem_signal(&q->sem_free_slots);   // Eliberează un loc
    return msg;
}
//Implementare Soft Timers
void rtos_timer_init(rtos_timer_t *timer, uint32_t period_ms, void (*callback)(void)) {
    timer->period_ticks = period_ms;
    timer->remaining_ticks = period_ms;
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
    // Returnează numărul de context switches (nu cicluri)
    return context_switch_cycles;
}

uint32_t rtos_get_max_context_switch_cycles(void) {
    return max_context_switch_cycles;
}

uint32_t rtos_get_isr_latency_cycles(void) {
    // Returnează diferența în tick-uri (ar trebui 1 mereu)
    return isr_latency_cycles;
}

uint32_t rtos_get_max_isr_latency_cycles(void) {
    return max_isr_latency_cycles;
}