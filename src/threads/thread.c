#include "threads/thread.h"
#include <debug.h>
#include <stddef.h>
#include <random.h>
#include <stdio.h>
#include <string.h>
#include "threads/flags.h"
#include "threads/interrupt.h"
#include "threads/intr-stubs.h"
#include "threads/palloc.h"
#include "threads/switch.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#include "threads/fixed_t.h"
#include "devices/timer.h"
#ifdef USERPROG
#include "userprog/process.h"
#endif

#define THREAD_MAGIC 0xcd6abf4b

/* Variável global do Advanced Scheduler */
fixed_t load_media; 

static struct list ready_list;
static struct list all_list;

static struct thread *idle_thread;
static struct thread *initial_thread;
static struct lock tid_lock;

struct kernel_thread_frame 
  {
    void *eip;                  
    thread_func *function;      
    void *aux;                  
  };

/* Estatísticas */
static long long idle_ticks;    
static long long kernel_ticks;  
static long long user_ticks;    

/* Escalonamento */
#define TIME_SLICE 4            
static unsigned thread_ticks;   

bool thread_mlfqs;

static void kernel_thread (thread_func *, void *aux);
static void idle (void *aux UNUSED);
static struct thread *running_thread (void);
static struct thread *next_thread_to_run (void);
static void init_thread (struct thread *, const char *name, int priority);
static bool is_thread (struct thread *) UNUSED;
static void *alloc_frame (struct thread *, size_t size);
static void schedule (void);
void thread_schedule_tail (struct thread *prev);
static tid_t allocate_tid (void);

/* Helpers estáticos para iteração do MLFQS */
static void thread_calculate_recent_cpu_helper (struct thread *t, void *aux UNUSED);
static void thread_calculate_priority_helper (struct thread *t, void *aux UNUSED);

void
thread_init (void) 
{
  ASSERT (intr_get_level () == INTR_OFF);

  lock_init (&tid_lock);
  list_init (&ready_list);
  list_init (&all_list);

  initial_thread = running_thread ();
  init_thread (initial_thread, "main", PRI_DEFAULT);
  initial_thread->status = THREAD_RUNNING;
  initial_thread->tid = allocate_tid ();

  if (thread_mlfqs)
    {
      load_media = 0; 
    }
}

void
thread_start (void) 
{
  struct semaphore idle_started;
  sema_init (&idle_started, 0);
  thread_create ("idle", PRI_MIN, idle, &idle_started);

  intr_enable ();
  sema_down (&idle_started);
}

/* Incrementa o recent_cpu da thread que está rodando (chamado a cada tick) */
void
thread_increment_recent_cpu (void)
{
  struct thread *cur = thread_current ();
  if (cur != idle_thread)
    {
      cur->recent_cpu = add_fixed_int (cur->recent_cpu, 1);
    }
}

/* Formula do load_avg (Executado a cada 1 segundo) */
void 
thread_calculate_load_avg (void)
{
  int ready_threads = list_size (&ready_list);
  if (thread_current () != idle_thread) 
    {
      ready_threads++;
    }

  fixed_t termo1 = mult_fixed (div_fixed (int_to_fixed (59), int_to_fixed (60)), load_media);
  fixed_t termo2 = mult_fixed_int (div_fixed (int_to_fixed (1), int_to_fixed (60)), ready_threads);

  load_media = add_fixed (termo1, termo2);
}

/* Handler individual do recent_cpu */
static void 
thread_calculate_recent_cpu_helper (struct thread *t, void *aux UNUSED)
{
  if (t == idle_thread) return;

  fixed_t dobro_load = mult_fixed_int (load_media, 2);
  fixed_t coeficiente = div_fixed (dobro_load, add_fixed_int (dobro_load, 1));
    
  t->recent_cpu = add_fixed_int (mult_fixed (coeficiente, t->recent_cpu), t->nice);
}

/* Recalcula recent_cpu de TODAS as threads */
void
thread_calculate_all_recent_cpu (void)
{
  thread_foreach (thread_calculate_recent_cpu_helper, NULL);
}

/* Handler individual da prioridade */
static void 
thread_calculate_priority_helper (struct thread *t, void *aux UNUSED)
{
  if (t == idle_thread) return;

  fixed_t penalty_cpu = div_fixed_int (t->recent_cpu, 4);
  int penalty_nice = t->nice * 2;
    
  int nova_prioridade = PRI_MAX - fixed_to_int_zero (penalty_cpu) - penalty_nice;

  if (nova_prioridade > PRI_MAX) nova_prioridade = PRI_MAX;
  if (nova_prioridade < PRI_MIN) nova_prioridade = PRI_MIN;

  t->priority = nova_prioridade;
} 

/* Recalcula a prioridade de TODAS as threads e reordena a fila */
void
thread_calculate_all_priorities (void)
{
  thread_foreach (thread_calculate_priority_helper, NULL);
  list_sort (&ready_list, thread_compare_priority, NULL);
}

void
thread_tick (void) 
{
  struct thread *t = thread_current ();

  if (t == idle_thread)
    idle_ticks++;
#ifdef USERPROG
  else if (t->pagedir != NULL)
    user_ticks++;
#endif
  else
    kernel_ticks++;

  /* Nota: Toda a lógica temporal complexa do MLFQS foi movida de 
     aqui para o timer_interrupt dentro do timer.c */

  if (++thread_ticks >= TIME_SLICE)
    intr_yield_on_return ();
}

void
thread_print_stats (void) 
{
  printf ("Thread: %lld idle ticks, %lld kernel ticks, %lld user ticks\n",
          idle_ticks, kernel_ticks, user_ticks);
}

tid_t
thread_create (const char *name, int priority,
               thread_func *function, void *aux) 
{
  struct thread *t;
  struct kernel_thread_frame *kf;
  struct switch_entry_frame *ef;
  struct switch_threads_frame *sf;
  tid_t tid;

  ASSERT (function != NULL);

  t = palloc_get_page (PAL_ZERO);
  if (t == NULL)
    return TID_ERROR;

  init_thread (t, name, priority);
  tid = t->tid = allocate_tid ();

  kf = alloc_frame (t, sizeof *kf);
  kf->eip = NULL;
  kf->function = function;
  kf->aux = aux;

  ef = alloc_frame (t, sizeof *ef);
  ef->eip = (void (*) (void)) kernel_thread;

  sf = alloc_frame (t, sizeof *sf);
  sf->eip = switch_entry;
  sf->ebp = 0;

  thread_unblock (t);
  thread_test_preempt (); 

  return tid;
}

void
thread_block (void) 
{
  ASSERT (!intr_context ());
  ASSERT (intr_get_level () == INTR_OFF);

  thread_current ()->status = THREAD_BLOCKED;
  schedule ();
}

void
thread_unblock (struct thread *t) 
{
  enum intr_level old_level;

  ASSERT (is_thread (t));

  old_level = intr_disable ();
  ASSERT (t->status == THREAD_BLOCKED);
  list_insert_ordered (&ready_list, &t->elem, thread_compare_priority, NULL); 
  t->status = THREAD_READY;
  intr_set_level (old_level);
}

const char *
thread_name (void) 
{
  return thread_current ()->name;
}

struct thread *
thread_current (void) 
{
  struct thread *t = running_thread ();
  ASSERT (is_thread (t));
  ASSERT (t->status == THREAD_RUNNING);
  return t;
}

tid_t
thread_tid (void) 
{
  return thread_current ()->tid;
}

void
thread_exit (void) 
{
  ASSERT (!intr_context ());

#ifdef USERPROG
  process_exit ();
#endif

  intr_disable ();
  list_remove (&thread_current()->allelem);
  thread_current ()->status = THREAD_DYING;
  schedule ();
  NOT_REACHED ();
}

void
thread_yield (void) 
{
  struct thread *cur = thread_current ();
  enum intr_level old_level;
  
  ASSERT (!intr_context ());

  old_level = intr_disable ();
  if (cur != idle_thread) 
    list_insert_ordered (&ready_list, &cur->elem, thread_compare_priority, NULL); 
  cur->status = THREAD_READY;
  schedule ();
  intr_set_level (old_level);
}

void
thread_foreach (thread_action_func *func, void *aux)
{
  struct list_elem *e;

  ASSERT (intr_get_level () == INTR_OFF);

  for (e = list_begin (&all_list); e != list_end (&all_list);
       e = list_next (e))
    {
      struct thread *t = list_entry (e, struct thread, allelem);
      func (t, aux);
    }
}

void
thread_set_priority (int new_priority) 
{
  if (thread_mlfqs) 
  {
    return;
  }

  thread_current ()->priority = new_priority;
  thread_test_preempt (); 
}

int
thread_get_priority (void) 
{
  return thread_current ()->priority;
}

void
thread_set_nice (int nice) 
{
  enum intr_level old_level = intr_disable (); 
  thread_current ()->nice = nice; 
  thread_calculate_priority_helper (thread_current (), NULL); 
  thread_test_preempt (); 
  intr_set_level (old_level); 
}

int
thread_get_nice (void) 
{
  return thread_current ()->nice;
}

int
thread_get_load_avg (void) 
{
  return fixed_to_int_round (mult_fixed_int (load_media, 100));
}

int
thread_get_recent_cpu (void) 
{
  return fixed_to_int_round (mult_fixed_int (thread_current ()->recent_cpu, 100));
}

bool 
thread_compare_priority (const struct list_elem *a, const struct list_elem *b, void *aux UNUSED) 
{
    struct thread *ta = list_entry (a, struct thread, elem);
    struct thread *tb = list_entry (b, struct thread, elem);
    return ta->priority > tb->priority; 
}

void 
thread_test_preempt (void) 
{
    if (!list_empty (&ready_list) && thread_current () != idle_thread) 
      {
        struct thread *next = list_entry (list_front (&ready_list), struct thread, elem);
        if (next->priority > thread_current ()->priority) 
          {
            if (intr_context ()) {
                intr_yield_on_return (); //para chamar de dentro do timer_interrupt
            } else {
                thread_yield (); //para chamar em fluxos normais
            }
          }
      }
}

static void
idle (void *idle_started_ UNUSED) 
{
  struct semaphore *idle_started = idle_started_;
  idle_thread = thread_current ();
  sema_up (idle_started);

  for (;;) 
    {
      intr_disable ();
      thread_block ();
      asm volatile ("sti; hlt" : : : "memory");
    }
}

static void
kernel_thread (thread_func *function, void *aux) 
{
  ASSERT (function != NULL);

  intr_enable ();       
  function (aux);       
  thread_exit ();       
}

struct thread *
running_thread (void) 
{
  uint32_t *esp;
  asm ("mov %%esp, %0" : "=g" (esp));
  return pg_round_down (esp);
}

static bool
is_thread (struct thread *t)
{
  return t != NULL && t->magic == THREAD_MAGIC;
}

static void
init_thread (struct thread *t, const char *name, int priority)
{
  enum intr_level old_level;

  ASSERT (t != NULL);
  ASSERT (PRI_MIN <= priority && priority <= PRI_MAX);
  ASSERT (name != NULL);

  memset (t, 0, sizeof *t);
  t->status = THREAD_BLOCKED;
  strlcpy (t->name, name, sizeof t->name);
  t->stack = (uint8_t *) t + PGSIZE;
  t->priority = priority;
  t->magic = THREAD_MAGIC;

  old_level = intr_disable ();
  list_push_back (&all_list, &t->allelem);
  intr_set_level (old_level);

  if (t == initial_thread) 
    { 
      t->nice = 0;
      t->recent_cpu = 0; 
    } 
  else 
    { 
      t->nice = thread_current ()->nice;
      t->recent_cpu = thread_current ()->recent_cpu;
    }
}

static void *
alloc_frame (struct thread *t, size_t size) 
{
  ASSERT (is_thread (t));
  ASSERT (size % sizeof (uint32_t) == 0);

  t->stack -= size;
  return t->stack;
}

static struct thread *
next_thread_to_run (void) 
{
  if (list_empty (&ready_list))
    return idle_thread;
  else
    return list_entry (list_pop_front (&ready_list), struct thread, elem);
}

void
thread_schedule_tail (struct thread *prev)
{
  struct thread *cur = running_thread ();
  
  ASSERT (intr_get_level () == INTR_OFF);

  cur->status = THREAD_RUNNING;
  thread_ticks = 0;

#ifdef USERPROG
  process_activate ();
#endif

  if (prev != NULL && prev->status == THREAD_DYING && prev != initial_thread) 
    {
      ASSERT (prev != cur);
      palloc_free_page (prev);
    }
}

static void
schedule (void) 
{
  struct thread *cur = running_thread ();
  struct thread *next = next_thread_to_run ();
  struct thread *prev = NULL;

  ASSERT (intr_get_level () == INTR_OFF);
  ASSERT (cur->status != THREAD_RUNNING);
  ASSERT (is_thread (next));

  if (cur != next)
    prev = switch_threads (cur, next);
  thread_schedule_tail (prev);
}

static tid_t
allocate_tid (void) 
{
  static tid_t next_tid = 1;
  tid_t tid;

  lock_acquire (&tid_lock);
  tid = next_tid++;
  lock_release (&tid_lock);

  return tid;
}

uint32_t thread_stack_ofs = offsetof (struct thread, stack);
