#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

/* Possible states of a thread: */
#define FREE 0x0
#define RUNNING 0x1
#define RUNNABLE 0x2

#define STACK_SIZE 8192
#define MAX_THREAD 4

struct context
{
  uint64 ra; // where to return (during first switching, switch to entry point)
  uint64 sp; // coroutine stack
  // callee-saved
  // In coroutine switching, the routine is thread_switch() away from switch and then =thread_switch() back and continue to execute,
  // so it is just like doing a normal blackbox function call, just save callee-saved registers according to the calling convention.
  uint64 s0;
  uint64 s1;
  uint64 s2;
  uint64 s3;
  uint64 s4;
  uint64 s5;
  uint64 s6;
  uint64 s7;
  uint64 s8;
  uint64 s9;
  uint64 s10;
  uint64 s11;
};

struct thread
{
  struct context ctx;     /* Registers (The state that needs to be saved when the coroutine is switched) */
  char stack[STACK_SIZE]; /* the thread's stack */
  int state;              /* FREE, RUNNING, RUNNABLE */
};
struct thread all_thread[MAX_THREAD];
struct thread *current_thread;
extern void thread_switch(uint64, uint64);

void thread_init(void)
{
  // main() is thread 0, which will make the first invocation to
  // thread_schedule().  it needs a stack so that the first thread_switch() can
  // save thread 0's state.  thread_schedule() won't run the main thread ever
  // again, because its state is set to RUNNING, and thread_schedule() selects
  // a RUNNABLE thread.
  current_thread = &all_thread[0];
  current_thread->state = RUNNING;
}

void thread_schedule(void)
{
  struct thread *t, *next_thread;

  /* Find another runnable thread. */
  next_thread = 0;
  t = current_thread + 1;
  for (int i = 0; i < MAX_THREAD; i++)
  {
    if (t >= all_thread + MAX_THREAD)
      t = all_thread;
    if (t->state == RUNNABLE)
    {
      next_thread = t;
      break;
    }
    t = t + 1;
  }

  // modifies something here, make it more reasonable...
  // if no runnable coroutine, schedue all_thread[0],
  // which is the first one calling thread_schedule()
  if (next_thread == 0)
  {
    printf("thread_schedule: no runnable threads\n");
    // exit(-1);
    next_thread = all_thread;
  }

  if (current_thread != next_thread)
  { /* switch threads?  */
    next_thread->state = RUNNING;
    t = current_thread;
    current_thread = next_thread;
    /* YOUR CODE HERE
     * Invoke thread_switch to switch from t to next_thread:
     * thread_switch(??, ??);
     */
    // Save the coroutine state here, and continue when switching back to the coroutine later
    // (think of switch as a normal function call, restoring sp, pc, and callee-saved sx registers)
    thread_switch((uint64)&t->ctx, (uint64)&current_thread->ctx);
  }
  else
  {
    next_thread = 0;
  }
}

void thread_create(void (*func)())
{
  struct thread *t;

  for (t = all_thread; t < all_thread + MAX_THREAD; t++)
  {
    if (t->state == FREE)
      break;
  }
  t->state = RUNNABLE;
  // YOUR CODE HERE
  // Important bug here....
  // initialize stack register, ON THE TOP!!!
  // i mistakenly set it to t->stack..., which points the bottom of the stack,
  // and latter writting in stack will override forward threads struct fields,
  // especially the thread.state, set a rubbish value into it which is not runable(0x2)
  t->ctx.sp = (uint64)t->stack + STACK_SIZE - 1;
  // When switching to this coroutine for the first time, ret (in thread_switch()) to the instr where ra (pc <- ra) is pointed and starts to execute.
  // so ra should store the entry point...
  t->ctx.ra = (uint64)func;
  t->state = RUNNABLE;
}

void thread_yield(void)
{
  // mark it runnable for latter switch
  current_thread->state = RUNNABLE;
  thread_schedule();
}

volatile int a_started, b_started, c_started;
volatile int a_n, b_n, c_n;

void thread_a(void)
{
  int i;
  printf("thread_a started\n");
  a_started = 1;
  while (b_started == 0 || c_started == 0)
    thread_yield();

  for (i = 0; i < 100; i++)
  {
    printf("thread_a %d\n", i);
    a_n += 1;
    thread_yield();
  }
  printf("thread_a: exit after %d\n", a_n);

  // this two line serves as coroutine exit...
  // i think it is better to encapsulate them as thread_exit()
  current_thread->state = FREE; // mark it free (shouldn't be scheduled)
  thread_schedule();            // switch away to schedule others
}

void thread_b(void)
{
  int i;
  printf("thread_b started\n");
  b_started = 1;
  while (a_started == 0 || c_started == 0)
    thread_yield();

  for (i = 0; i < 100; i++)
  {
    printf("thread_b %d\n", i);
    b_n += 1;
    thread_yield();
  }
  printf("thread_b: exit after %d\n", b_n);

  current_thread->state = FREE;
  thread_schedule();
}

void thread_c(void)
{
  int i;
  printf("thread_c started\n");
  c_started = 1;
  while (a_started == 0 || b_started == 0)
    thread_yield();

  for (i = 0; i < 100; i++)
  {
    printf("thread_c %d\n", i);
    c_n += 1;
    thread_yield();
  }
  printf("thread_c: exit after %d\n", c_n);

  current_thread->state = FREE;
  thread_schedule();
}

int main(int argc, char *argv[])
{
  a_started = b_started = c_started = 0;
  a_n = b_n = c_n = 0;
  thread_init();
  thread_create(thread_a);
  thread_create(thread_b);
  thread_create(thread_c);

  // main calls the thread_schedule() and triggered all coroutine scheduling...
  // at first, current_thread = all_thread[0], so main()'s register and stack state will be stored in all_thread[0]
  // through thread_schedule()->thread_switch(). that is to say, main() is the first coroutine...
  // however, main() is not be called latter, why?
  // there is a little tricky thing...
  // all_thread[0] is marked as running... and will not be set as runnable (need call thread_yield()),
  // so only all the others are not runable, just schedule all_thread[0] and then we can return back here!!!
  thread_schedule();
  exit(0);
}
