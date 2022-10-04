// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run
{
  struct run *next;
};

struct
{
  struct spinlock lock;
  struct run *freelist;
} kmem[NCPU];

struct spinlock steal_pg_lock;

void kinit()
{
  // init each locks
  char lock_name[] = "kmemlk_0";
  for (int i = 0; i < NCPU; ++i)
  {
    lock_name[7] = i + '0';
    initlock(&kmem[i].lock, lock_name);
  }
  // free all pages and append to kmem[0]
  freerange(end, (void *)PHYSTOP);
}

void freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char *)PGROUNDUP((uint64)pa_start);
  for (; p + PGSIZE <= (char *)pa_end; p += PGSIZE)
    kfree(p);
}

// Free the page of physical memory pointed at by v,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void kfree(void *pa)
{
  struct run *r;

  if (((uint64)pa % PGSIZE) != 0 || (char *)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run *)pa;

  push_off();
  uint8 id = cpuid();

  acquire(&kmem[id].lock);
  r->next = kmem[id].freelist;
  kmem[id].freelist = r;
  release(&kmem[id].lock);

  pop_off();
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;

  // cpuid() returns current cpu id, whill using this id, process shouldn't be scheduled,
  // if scheduled, other cpu may schedule this process next time, which makes cpu id wrong in the following code.
  // Also, a good thing is that, disabling time interrupt by push_off() makes the CPU always run the current process,
  // no need to consider the concurrency problem caused by time-sharing multiplexing on the same CPU
  push_off();
  uint8 current_id = cpuid();

  acquire(&kmem[current_id].lock);
  r = kmem[current_id].freelist;
  // no free page
  if (!r)
  {
    // give up current lock first to avoid deadlock and then try to steal
    // possible deadlock:
    //  cpu A: lock A -> steal lock  -> lock B (stuck)
    //  cpu B: lock B     -> steal lock (stuck)
    release(&kmem[current_id].lock);

    // only one proc can steal page at the same time
    acquire(&steal_pg_lock);
    // get current lock again
    acquire(&kmem[current_id].lock);

    // how many page need to steal, total is STEALPAGENUM
    uint8 to_steal_num = NSTEALPAGE;
    for (uint8 id = 0; id < NCPU; ++id)
    {
      if (to_steal_num == 0)
        break;

      if (id == current_id)
        continue;

      acquire(&kmem[id].lock);

      // steal to_steal_num in kmem[id]
      for (int i = 0; i < to_steal_num; ++i)
      {
        r = kmem[id].freelist;
        if (r)
        {
          kmem[id].freelist = r->next;
          r->next = kmem[current_id].freelist;
          kmem[current_id].freelist = r;
          --to_steal_num; // successfully steal one page
        }
      }

      release(&kmem[id].lock);
    }

    // finished page-steal process
    release(&steal_pg_lock);
    r = kmem[current_id].freelist;
  }

  // if this cpu has free page (kmem[current_id] is not NULL), allocate this page
  if (r)
    kmem[current_id].freelist = r->next;

  release(&kmem[current_id].lock);

  pop_off();

  if (r)
    memset((char *)r, 5, PGSIZE); // fill with junk

  return (void *)r;
}
