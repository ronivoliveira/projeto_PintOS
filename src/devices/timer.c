#include "devices/timer.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include "devices/pit.h"
#include "threads/interrupt.h"
#include "threads/synch.h"
#include "threads/thread.h"
#include "threads/fixed_t.h" // Garante o suporte à aritmética de ponto fixo do MLFQS
  
/* See [8254] for hardware details of the 8254 timer chip. */

#if TIMER_FREQ < 19
#error 8254 timer requires TIMER_FREQ >= 19
#endif
#if TIMER_FREQ > 1000
#error TIMER_FREQ <= 1000 recommended
#endif

/* Number of timer ticks since OS booted. */
static int64_t ticks;

/* Number of loops per timer tick.
   Initialized by timer_calibrate(). */
static unsigned loops_per_tick;

/* Lista global que armazena as threads bloqueadas pelo timer_sleep */
static struct list threads_dormindo;

static intr_handler_func timer_interrupt;
static bool too_many_loops (unsigned loops);
static void busy_wait (int64_t loops);
static void real_time_sleep (int64_t num, int32_t denom);
static void real_time_delay (int64_t num, int32_t denom);


/* Sets up the timer to interrupt TIMER_FREQ times per second,
   and registers the corresponding interrupt. */
void
timer_init (void) 
{
  pit_configure_channel (0, 2, TIMER_FREQ);
  intr_register_ext (0x20, timer_interrupt, "8254 Timer");

  /* Inicializa a lista de threads adormecidas (Alarm Clock) */
  list_init (&threads_dormindo);
}

/* Calibrates loops_per_tick, used to implement brief delays. */
void
timer_calibrate (void) 
{
  unsigned high_bit, test_bit;

  ASSERT (intr_get_level () == INTR_ON);
  printf ("Calibrating timer...  ");

  /* Approximate loops_per_tick as the largest power-of-two
     still less than one timer tick. */
  loops_per_tick = 1u << 10;
  while (!too_many_loops (loops_per_tick << 1)) 
    {
      loops_per_tick <<= 1;
      ASSERT (loops_per_tick != 0);
    }

  /* Refine the next 8 bits of loops_per_tick. */
  high_bit = loops_per_tick;
  for (test_bit = high_bit >> 1; test_bit != high_bit >> 10; test_bit >>= 1)
    if (!too_many_loops (loops_per_tick | test_bit))
      loops_per_tick |= test_bit;

  printf ("%'"PRIu64" loops/s.\n", (uint64_t) loops_per_tick * TIMER_FREQ);
}

/* Returns the number of timer ticks since the OS booted. */
int64_t
timer_ticks (void) 
{
  enum intr_level old_level = intr_disable ();
  int64_t t = ticks;
  intr_set_level (old_level);
  return t;
}

/* Returns the number of timer ticks elapsed since THEN, which
   should be a value once returned by timer_ticks(). */
int64_t
timer_elapsed (int64_t then) 
{
  return timer_ticks () - then;
}

/* Sleeps for approximately TICKS timer ticks.  Interrupts must
   be turned on. */
void
timer_sleep (int64_t ticks) 
{
  if (ticks < 0) 
    return;

  ASSERT (intr_get_level () == INTR_ON);

  /* Desabilita as interrupções para garantir atomicidade ao mexer na lista */
  enum intr_level old_level = intr_disable ();   

  int64_t start = timer_ticks ();
  struct thread *atual = thread_current();

  /* Define o momento exato em que a thread deve acordar */
  atual->ticks_acordar = start + ticks; 
  
  /* Insere na lista e bloqueia o estado da thread */
  list_push_back(&threads_dormindo, &atual->elem);
  thread_block();

  /* Restaura o nível anterior de interrupção */
  intr_set_level(old_level); 
}

/* Sleeps for approximately MS milliseconds.  Interrupts must be
   turned on. */
void
timer_msleep (int64_t ms) 
{
  real_time_sleep (ms, 1000);
}

/* Sleeps for approximately US microseconds.  Interrupts must be
   turned on. */
void
timer_usleep (int64_t us) 
{
  real_time_sleep (us, 1000 * 1000);
}

/* Sleeps for approximately NS nanoseconds.  Interrupts must be
   turned on. */
void
timer_nsleep (int64_t ns) 
{
  real_time_sleep (ns, 1000 * 1000 * 1000);
}

/* Busy-waits for approximately MS milliseconds.  Interrupts need
   not be turned on. */
void
timer_mdelay (int64_t ms) 
{
  real_time_delay (ms, 1000);
}

/* Sleeps for approximately US microseconds.  Interrupts need not
   be turned on. */
void
timer_udelay (int64_t us) 
{
  real_time_delay (us, 1000 * 1000);
}

/* Sleeps execution for approximately NS nanoseconds.  Interrupts
   need not be turned on.*/
void
timer_ndelay (int64_t ns) 
{
  real_time_delay (ns, 1000 * 1000 * 1000);
}

/* Prints timer statistics. */
void
timer_print_stats (void) 
{
  printf ("Timer: %"PRId64" ticks\n", timer_ticks ());
}

/* Timer interrupt handler. */
static void
timer_interrupt (struct intr_frame *args UNUSED)
{
  ticks++;
  thread_tick ();

  /* -------------------------------------------------------------
   * BLOCO ADVANCED SCHEDULING (MLFQS)
   * ------------------------------------------------------------- */
  if (thread_mlfqs)
    {
      /* Incrementa o recent_cpu da thread que está rodando atualmente */
      thread_increment_recent_cpu ();

      /* A cada 1 segundo (quantidade de ticks igual a TIMER_FREQ),
         recalculamos o load_avg e o recent_cpu de TODAS as threads */
      if (ticks % TIMER_FREQ == 0)
        {
          thread_calculate_load_avg ();
          thread_calculate_all_recent_cpu ();
        }

      /* A cada 4 ticks, recalculamos as prioridades dinâmicas */
      if (ticks % 4 == 0)
        {
          thread_calculate_all_priorities ();
        }
    }

  /* -------------------------------------------------------------
   * BLOCO ALARM CLOCK
   * ------------------------------------------------------------- */
  struct list_elem *thread_atual = list_begin(&threads_dormindo);
  while (thread_atual != list_end(&threads_dormindo))
    {
      struct thread *thread = list_entry(thread_atual, struct thread, elem);
      
      /* Se os ticks do sistema alcançaram o tempo estipulado, acorda a thread */
      if (ticks >= thread->ticks_acordar)
        {
          thread_atual = list_remove(thread_atual);
          thread_unblock(thread);
        }
      else
        {
          thread_atual = list_next(thread_atual);
        }
    }
}

/* Returns true if LOOPS iterations waits for more than one timer
   tick, otherwise false. */
static bool
too_many_loops (unsigned loops) 
{
  /* Wait for a timer tick. */
  int64_t start = ticks;
  while (ticks == start)
    barrier ();

  /* Run LOOPS loops. */
  start = ticks;
  busy_wait (loops);

  /* If the tick count changed, we iterated too long. */
  barrier ();
  return start != ticks;
}

/* Iterates through a simple loop LOOPS times, for implementing
   brief delays. */
static void NO_INLINE
busy_wait (int64_t loops) 
{
  while (loops-- > 0)
    barrier ();
}

/* Sleep for approximately NUM/DENOM seconds. */
static void
real_time_sleep (int64_t num, int32_t denom) 
{
  int64_t ticks = num * TIMER_FREQ / denom;

  ASSERT (intr_get_level () == INTR_ON);
  if (ticks > 0) 
    {
      timer_sleep (ticks); 
    }
  else 
    {
      real_time_delay (num, denom); 
    }
}

/* Busy-wait for approximately NUM/DENOM seconds. */
static void
real_time_delay (int64_t num, int32_t denom)
{
  ASSERT (denom % 1000 == 0);
  busy_wait (loops_per_tick * num / 1000 * TIMER_FREQ / (denom / 1000)); 
}
