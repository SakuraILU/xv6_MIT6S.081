// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"
#include "proc.h"

void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

#define PA2PGID(x) (((uint64)x) >> PGSHIFT)
#define PGCOUNT_LEN (PA2PGID(PHYSTOP))
struct
{
  struct spinlock lock;
  uint64 count;
} pg_counter[PGCOUNT_LEN];

struct run
{
  struct run *next;
};

struct
{
  struct spinlock lock;
  struct run *freelist;
} kmem;

void kinit()
{
  initlock(&kmem.lock, "kmem");

  for (uint64 i = 0; i < PGCOUNT_LEN; ++i)
  {
    // freerange() use kfree() to establish whole free page link list
    // to get real free (rather than just decrease count), need to
    // set all the counters to 1 for freerange()
    // if not init to 1 (default 0 in global vars),
    // (--pg_counter[PA2PGID(pa)].count) in freerange will be UINT_MAX...and cause trouble
    pg_counter[i].count = 1;
    initlock(&pg_counter[i].lock, "pglock");
  }

  // establish whole free page link list
  freerange(end, (void *)PHYSTOP);
}

void freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char *)PGROUNDUP((uint64)pa_start);
  for (; p + PGSIZE <= (char *)pa_end; p += PGSIZE)
    kfree(p);
}

/// @brief
// Decrease the physical page's count by 1, and free this page only when count decresed to 0.
// It's more like an unlink operation? or we can call it a shallow free.
/// @param pa physical page
void kfree(void *pa)
{
  // printf("kfree:s\n");

  if (((uint64)pa % PGSIZE) != 0 || (char *)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  // decrease coun by 1
  acquire(&pg_counter[PA2PGID(pa)].lock);
  if ((--pg_counter[PA2PGID(pa)].count) > 0)
  {
    release(&pg_counter[PA2PGID(pa)].lock);
    return;
  }
  release(&pg_counter[PA2PGID(pa)].lock);

  // Fill with junk to catch dangling refs.
  struct run *r;
  memset(pa, 1, PGSIZE);

  r = (struct run *)pa;

  acquire(&kmem.lock);
  r->next = kmem.freelist;
  kmem.freelist = r;
  release(&kmem.lock);
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;

  acquire(&kmem.lock);

  r = kmem.freelist;
  if (r)
  {
    kmem.freelist = r->next;
  }
  release(&kmem.lock);

  if (r)
  {
    memset((char *)r, 5, PGSIZE); // fill with junk
    // Initially allocate a new physical page,
    // the allocation process will not be concurrent with any page operation.
    // e.g. kfree() and kpage_ref_inc() only applies to a exist page.
    pg_counter[PA2PGID(r)].count = 1;
  }
  return (void *)r;
}

/// @brief increase the kernel page's counter by 1
/// @param pa page's physical adress
void kpage_ref_inc(void *pa)
{
  acquire(&pg_counter[PA2PGID(pa)].lock);
  pg_counter[PA2PGID(pa)].count++;
  release(&pg_counter[PA2PGID(pa)].lock);
}
