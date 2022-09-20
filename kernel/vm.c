#include "param.h"
#include "types.h"
#include "memlayout.h"
#include "elf.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
/*
 * the kernel's page table.
 */
pagetable_t kernel_pagetable;

extern char etext[]; // kernel.ld sets this to end of kernel code.

extern char trampoline[]; // trampoline.S

static void vmprintf_dfs(pagetable_t pagetable, uint8 depth);
/*
 * create a direct-map page table for the kernel.
 */
void kvminit()
{
  kernel_pagetable = (pagetable_t)kalloc();
  memset(kernel_pagetable, 0, PGSIZE);

  // uart registers
  kvmmap(UART0, UART0, PGSIZE, PTE_R | PTE_W);

  // virtio mmio disk interface
  kvmmap(VIRTIO0, VIRTIO0, PGSIZE, PTE_R | PTE_W);

  // CLINT
  kvmmap(CLINT, CLINT, 0x10000, PTE_R | PTE_W);

  // PLIC
  kvmmap(PLIC, PLIC, 0x400000, PTE_R | PTE_W);

  // map kernel text executable and read-only.
  kvmmap(KERNBASE, KERNBASE, (uint64)etext - KERNBASE, PTE_R | PTE_X);

  // map kernel data and the physical RAM we'll make use of.
  kvmmap((uint64)etext, (uint64)etext, PHYSTOP - (uint64)etext, PTE_R | PTE_W);

  // map the trampoline for trap entry/exit to
  // the highest virtual address in the kernel.
  kvmmap(TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);
}

/// @brief init the kernel pagetable for a single process.
///        this function will not include the CLINT part, which is only needed for timer initialization
//         by the global kernel pgtbl when booting the kernel
/// @param pagetable process's kernel pagetable, need to be kalloced before calling
void kvminitproc(pagetable_t pagetable)
{
  // uart registers
  kvmmapproc(pagetable, UART0, UART0, PGSIZE, PTE_R | PTE_W);

  // virtio mmio disk interface
  kvmmapproc(pagetable, VIRTIO0, VIRTIO0, PGSIZE, PTE_R | PTE_W);

  // PLIC
  kvmmapproc(pagetable, PLIC, PLIC, 0x400000, PTE_R | PTE_W);

  // mapproc kernel text executable and read-only.
  kvmmapproc(pagetable, KERNBASE, KERNBASE, (uint64)etext - KERNBASE, PTE_R | PTE_X);

  // mapproc kernel data and the physical RAM we'll make use of.
  kvmmapproc(pagetable, (uint64)etext, (uint64)etext, PHYSTOP - (uint64)etext, PTE_R | PTE_W);

  // mapproc the trampoline for trap entry/exit to
  // the highest virtual address in the kernel.
  kvmmapproc(pagetable, TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);
}

// Switch h/w page table register to the kernel's page table,
// and enable paging.
void kvminithart()
{
  // write process's kernel_pgtbl's adress to satp register
  w_satp(MAKE_SATP(kernel_pagetable));
  // flush TLB cache
  sfence_vma();
}

/// @brief Switch h/w page table register to the process's kernel page table and enable paging
/// @param pagetable process's kernel pagetable
void kvminithartproc(pagetable_t pagetable)
{
  // write process's kernel_pgtbl's adress to satp register
  w_satp(MAKE_SATP(pagetable));
  // flush TLB cache
  sfence_vma();
}

// Return the address of the PTE in page table pagetable
// that corresponds to virtual address va.  If alloc!=0,
// create any required page-table pages.
//
// The risc-v Sv39 scheme has three levels of page-table
// pages. A page-table page contains 512 64-bit PTEs.
// A 64-bit virtual address is split into five fields:
//   39..63 -- must be zero.
//   30..38 -- 9 bits of level-2 index.
//   21..29 -- 9 bits of level-1 index.
//   12..20 -- 9 bits of level-0 index.
//    0..11 -- 12 bits of byte offset within the page.
pte_t *
walk(pagetable_t pagetable, uint64 va, int alloc)
{
  if (va >= MAXVA)
    panic("walk");
  // traverse pgtbl's level2 and level1 and reach final level 0
  for (int level = 2; level > 0; level--)
  {
    // PX macro: get this level's pgtbl index according to vaddr
    pte_t *pte = &pagetable[PX(level, va)];
    // pte is valid: has pgtbl child
    if (*pte & PTE_V)
    {
      // PTE2PA macro: get child pagetable from pgtbl entry
      // [54-11] is the page-segment adress of child pgtbl, so append 12 zeros and get it's actually adress
      pagetable = (pagetable_t)PTE2PA(*pte);
    }
    else
    {
      // if pte is not valid

      // if set not alloc or kalloc failed, return 0;
      if (!alloc || (pagetable = (pde_t *)kalloc()) == 0)
        return 0;
      // can alloc and kalloc success, add this pgtbl child and set pte to valid
      memset(pagetable, 0, PGSIZE);
      *pte = PA2PTE(pagetable) | PTE_V;
    }
  }
  // return level 0 's pte, which refers the physical adress
  return &pagetable[PX(0, va)];
}

// Look up a virtual address, return the physical address,
// or 0 if not mapped.
// Can only be used to look up user pages.
uint64
walkaddr(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;
  uint64 pa;

  if (va >= MAXVA)
    return 0;
  // get level 0 's pte
  pte = walk(pagetable, va, 0);

  // check flags on this pte
  if (pte == 0)
    return 0;
  // PTE_V: is a valid pgtbl entry? if not, return null
  if ((*pte & PTE_V) == 0)
    return 0;
  // PTE_U: is a user_pgtbl entry? if not, return null
  if ((*pte & PTE_U) == 0)
    return 0;

  // return physical adress
  pa = PTE2PA(*pte);
  return pa;
}

// add a mapping to the kernel page table.
// only used when booting.
// does not flush TLB or enable paging.
void kvmmap(uint64 va, uint64 pa, uint64 sz, int perm)
{
  if (mappages(kernel_pagetable, va, sz, pa, perm) != 0)
    panic("kvmmap");
}

/// @brief
//  map virtual adress [va, va+sz-1] to physical adress [pa, pa+sz-1]
/// @param pagetable process's kernel pagetable
/// @param va virtual adress
/// @param size mapping range is [va, va + size - 1]
/// @param pa physical adress which needed to be kalloced before calling
/// @param perm permmssion flags, like PTE_R | PTE_W | PTE_X | PTE_U
void kvmmapproc(pagetable_t pagetable, uint64 va, uint64 pa, uint64 sz, int perm)
{
  if (mappages(pagetable, va, sz, pa, perm) != 0)
    panic("kvmmapproc");
}

// translate a kernel virtual address to
// a physical address. only needed for
// addresses on the stack.
// assumes va is page aligned.
uint64 kvmpa(uint64 va)
{
  uint64 off = va % PGSIZE;
  pte_t *pte;
  uint64 pa;

  pte = walk(kernel_pagetable, va, 0);
  if (pte == 0)
    panic("kvmpa");
  if ((*pte & PTE_V) == 0)
    panic("kvmpa");
  pa = PTE2PA(*pte);
  return pa + off;
}

uint64 kvmpaproc(pagetable_t pagetable, uint64 va)
{

  uint64 off = va % PGSIZE;
  pte_t *pte;
  uint64 pa;

  pte = walk(pagetable, va, 0);
  if (pte == 0)
    panic("kvmpa");
  if ((*pte & PTE_V) == 0)
    panic("kvmpa");
  pa = PTE2PA(*pte);
  return pa + off;
}

/// @brief
// Create PTEs for virtual addresses starting at va that refer to
// physical addresses starting at pa. va and size might not
// be page-aligned (they will be round down).
/// @param pagetable
/// @param va virtual adress
/// @param size mapping range is [va, va + size - 1]
/// @param pa physical adress which needed to be kalloced before calling
/// @param perm permmssion flags, like PTE_R | PTE_W | PTE_X | PTE_U
/// @return Returns 0 on success, -1 if walk() couldn't allocate a needed page-table page.
int mappages(pagetable_t pagetable, uint64 va, uint64 size, uint64 pa, int perm)
{
  uint64 a, last;
  pte_t *pte;
  a = PGROUNDDOWN(va);
  last = PGROUNDDOWN(va + size - 1);
  for (;;)
  {
    // get level 0 's pte, which refers physical adress, return 0 on error,
    // also, this explains why NULL == 0...
    if ((pte = walk(pagetable, a, 1)) == 0)
      return -1;
    // should map a invalid adress...if set PTE_V, means there is already mapped a physical page
    if (*pte & PTE_V)
      panic("remap");
    // PA2PTE: convert physical adress to pgtbl format: write pa's segment adress[55:12] in pte[53:10], first >> 12 and then << 10
    //         |reserved|pa's segment adress | other flags |valid flags|
    //          63        53               10 9           1      0
    *pte = PA2PTE(pa) | perm | PTE_V;
    if (a == last)
      break;

    // map the upper page in next loop
    a += PGSIZE;
    pa += PGSIZE;
  }
  return 0;
}

// Remove npages of mappings starting from va. va must be
// page-aligned. The mappings must exist.
// Optionally free the physical memory.
void uvmunmap(pagetable_t pagetable, uint64 va, uint64 npages, int do_free)
{
  uint64 a;
  pte_t *pte;

  if ((va % PGSIZE) != 0)
    panic("uvmunmap: not aligned");

  for (a = va; a < va + npages * PGSIZE; a += PGSIZE)
  {
    // printf("unmap va %p\n", a);
    if ((pte = walk(pagetable, a, 0)) == 0)
      panic("uvmunmap: walk");
    if ((*pte & PTE_V) == 0)
      panic("uvmunmap: not mapped");
    if (PTE_FLAGS(*pte) == PTE_V)
      panic("uvmunmap: not a leaf");
    if (do_free)
    {
      uint64 pa = PTE2PA(*pte);
      kfree((void *)pa);
    }
    *pte = 0;
  }
}
// create an empty user page table.
// returns 0 if out of memory.
pagetable_t
uvmcreate()
{
  pagetable_t pagetable;
  pagetable = (pagetable_t)kalloc();
  if (pagetable == 0)
    return 0;
  memset(pagetable, 0, PGSIZE);
  return pagetable;
}

// Load the user initcode into address 0 of pagetable,
// for the very first process.
// sz must be less than a page.
void uvminit(pagetable_t pagetable, uchar *src, uint sz)
{
  char *mem;

  if (sz >= PGSIZE)
    panic("inituvm: more than a page");
  mem = kalloc();
  memset(mem, 0, PGSIZE);
  mappages(pagetable, 0, PGSIZE, (uint64)mem, PTE_W | PTE_R | PTE_X | PTE_U);
  memmove(mem, src, sz);
}

// Allocate PTEs and physical memory to grow process from oldsz to
// newsz, which need not be page aligned.  Returns new size or 0 on error.
uint64
uvmalloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz)
{
  char *mem;
  uint64 a;

  if (newsz < oldsz)
    return oldsz;

  oldsz = PGROUNDUP(oldsz);
  for (a = oldsz; a < newsz; a += PGSIZE)
  {
    mem = kalloc();
    if (mem == 0)
    {
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
    memset(mem, 0, PGSIZE);
    if (mappages(pagetable, a, PGSIZE, (uint64)mem, PTE_W | PTE_X | PTE_R | PTE_U) != 0)
    {
      kfree(mem);
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
  }
  return newsz;
}

// Deallocate user pages to bring the process size from oldsz to
// newsz.  oldsz and newsz need not be page-aligned, nor does newsz
// need to be less than oldsz.  oldsz can be larger than the actual
// process size.  Returns the new process size.
uint64
uvmdealloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz)
{
  if (newsz >= oldsz)
    return oldsz;

  if (PGROUNDUP(newsz) < PGROUNDUP(oldsz))
  {
    int npages = (PGROUNDUP(oldsz) - PGROUNDUP(newsz)) / PGSIZE;
    uvmunmap(pagetable, PGROUNDUP(newsz), npages, 1);
  }

  return newsz;
}

uint64 kvmdealloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz)
{
  if (newsz >= oldsz)
    return oldsz;

  if (PGROUNDUP(newsz) < PGROUNDUP(oldsz))
  {
    int npages = (PGROUNDUP(oldsz) - PGROUNDUP(newsz)) / PGSIZE;
    uvmunmap(pagetable, PGROUNDUP(newsz), npages, 0);
  }

  return newsz;
}

// Recursively free page-table pages.
// All leaf mappings must already have been removed.
void freewalk(pagetable_t pagetable)
{
  // there are 2^9 = 512 PTEs in a page table.
  for (int i = 0; i < 512; i++)
  {
    pte_t pte = pagetable[i];
    if ((pte & PTE_V) && (pte & (PTE_R | PTE_W | PTE_X)) == 0)
    {
      // this PTE points to a lower-level page table.
      uint64 child = PTE2PA(pte);
      freewalk((pagetable_t)child);
      pagetable[i] = 0;
    }
    else if (pte & PTE_V)
    {
      panic("freewalk: leaf");
    }
  }
  kfree((void *)pagetable);
}

/// @brief
// Recursively free process's whole kernel pagetable.
// will not free physical pages, it's shallow free...
/// @param pagetable process's kernel pagetable
void kvmfree(pagetable_t pagetable)
{
  // there are 2^9 = 512 PTEs in a page table.
  for (int i = 0; i < 512; i++)
  {
    pte_t pte = pagetable[i];
    if ((pte & PTE_V) && (pte & (PTE_R | PTE_W | PTE_X)) == 0)
    {
      // this PTE points to a lower-level page table.
      uint64 child = PTE2PA(pte);
      kvmfree((pagetable_t)child);
    }
  }
  kfree((void *)pagetable);
}

// Free user memory pages,
// then free page-table pages.
void uvmfree(pagetable_t pagetable, uint64 sz)
{
  if (sz > 0)
    uvmunmap(pagetable, 0, PGROUNDUP(sz) / PGSIZE, 1);
  freewalk(pagetable);
}

// Given a parent process's page table, copy
// its memory into a child's page table.
// Copies both the page table and the
// physical memory. We can call it as deep copy
// returns 0 on success, -1 on failure.
// frees any allocated pages on failure.
int uvmcopy(pagetable_t old, pagetable_t new, uint64 sz)
{
  pte_t *pte;
  uint64 pa, i;
  uint flags;
  char *mem;

  // printf("uvm map start from %p to %p\n", 0, sz);
  for (i = 0; i < sz; i += PGSIZE)
  {
    if ((pte = walk(old, i, 0)) == 0)
      panic("uvmcopy: pte should exist");
    if ((*pte & PTE_V) == 0)
      panic("uvmcopy: page not present");
    pa = PTE2PA(*pte);
    flags = PTE_FLAGS(*pte);
    if ((mem = kalloc()) == 0)
      goto err;
    memmove(mem, (char *)pa, PGSIZE);
    // printf("uvmmap va %p to %pa\n", i, mem);
    if (mappages(new, i, PGSIZE, (uint64)mem, flags) != 0)
    {
      kfree(mem);
      goto err;
    }
  }
  // printf("unmap finished========");
  return 0;

err:
  uvmunmap(new, 0, i / PGSIZE, 1);
  return -1;
}

/// @brief
// Given the process's user pagetable, copy
// it into it's kernel pagetable.
// Only copies the pagetable and not copies the
// physical memory. We can call it as shallow copy
/// @param user_pgtbl process's user pagetable
/// @param kernel_pgtbl process's kernel pagetable
/// @param va the start of virutal adress range
/// @param va_end the end of virtual adress range
/// @return returns 0 on success, -1 on failure. frees any allocated pages on failure.
int kvmcopy(pagetable_t user_pgtbl, pagetable_t kernel_pgtbl, uint64 va, uint64 va_end)
{
  pte_t *pte;
  uint64 mem, i;
  uint flags;
  va = PGROUNDUP(va);
  va_end = PGROUNDUP(va_end);
  for (i = va; i < va_end; i += PGSIZE)
  {
    if ((pte = walk(user_pgtbl, i, 0)) == 0)
      panic("uvmcopy: pte should exist");
    if ((*pte & PTE_V) == 0)
      panic("uvmcopy: page not present");
    mem = PTE2PA(*pte);
    flags = PTE_FLAGS(*pte);
    flags &= (~PTE_U);
    if (mappages(kernel_pgtbl, i, PGSIZE, mem, flags) != 0)
    {
      goto err;
    }
  }
  return 0;
err:

  kvmdealloc(kernel_pgtbl, i, va);
  return -1;
}

// mark a PTE invalid for user access.
// used by exec for the user stack guard page.
void uvmclear(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;

  pte = walk(pagetable, va, 0);
  if (pte == 0)
    panic("uvmclear");
  *pte &= ~PTE_U;
}

// Copy from kernel to user.
// Copy len bytes from src to virtual address dstva in a given page table.
// Return 0 on success, -1 on error.
int copyout(pagetable_t pagetable, uint64 dstva, char *src, uint64 len)
{
  uint64 n, va0, pa0;

  while (len > 0)
  {
    va0 = PGROUNDDOWN(dstva);
    pa0 = walkaddr(pagetable, va0);
    if (pa0 == 0)
      return -1;
    n = PGSIZE - (dstva - va0);
    if (n > len)
      n = len;
    memmove((void *)(pa0 + (dstva - va0)), src, n);

    len -= n;
    src += n;
    dstva = va0 + PGSIZE;
  }
  return 0;
}

// Copy from user to kernel.
// Copy len bytes to dst from virtual address srcva in a given page table.
// Return 0 on success, -1 on error.
int copyin(pagetable_t pagetable, char *dst, uint64 srcva, uint64 len)
{
  return copyin_new(pagetable, dst, srcva, len);
}

// Copy a null-terminated string from user to kernel.
// Copy bytes to dst from virtual address srcva in a given page table,
// until a '\0', or max.
// Return 0 on success, -1 on error.
int copyinstr(pagetable_t pagetable, char *dst, uint64 srcva, uint64 max)
{
  return copyinstr_new(pagetable, dst, srcva, max);
}

void vmprintf(pagetable_t pagetable)
{
  printf("page table %p\n", pagetable);
  vmprintf_dfs(pagetable, 1);
}

static void vmprintf_dfs(pagetable_t pagetable, uint8 depth)
{
  // there are 2^9 = 512 PTEs in a page table.
  for (int i = 0; i < 512; i++)
  {
    pte_t pte = pagetable[i];
    if (!(pte && PTE_V))
      continue;

    for (uint8 i = 0; i < depth; ++i)
    {
      printf("..");
    }
    printf("%d: pte %p pa %p\n", i, pte, PTE2PA(pte));
    if ((pte & PTE_V) && (pte & (PTE_R | PTE_W | PTE_X)) == 0)
    {

      // this PTE points to a lower-level page table.
      uint64 child = PTE2PA(pte);
      vmprintf_dfs((pagetable_t)child, depth + 1);
    }
  }
}