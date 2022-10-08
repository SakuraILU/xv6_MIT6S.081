#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "date.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"
#include "fcntl.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"

uint64
sys_exit(void)
{
  int n;
  if (argint(0, &n) < 0)
    return -1;
  exit(n);
  return 0; // not reached
}

uint64
sys_getpid(void)
{
  return myproc()->pid;
}

uint64
sys_fork(void)
{
  return fork();
}

uint64
sys_wait(void)
{
  uint64 p;
  if (argaddr(0, &p) < 0)
    return -1;
  return wait(p);
}

uint64
sys_sbrk(void)
{
  int addr;
  int n;

  if (argint(0, &n) < 0)
    return -1;
  addr = myproc()->sz;
  if (growproc(n) < 0)
    return -1;
  return addr;
}

uint64
sys_sleep(void)
{
  int n;
  uint ticks0;

  if (argint(0, &n) < 0)
    return -1;
  acquire(&tickslock);
  ticks0 = ticks;
  while (ticks - ticks0 < n)
  {
    if (myproc()->killed)
    {
      release(&tickslock);
      return -1;
    }
    sleep(&ticks, &tickslock);
  }
  release(&tickslock);
  return 0;
}

uint64
sys_kill(void)
{
  int pid;

  if (argint(0, &pid) < 0)
    return -1;
  return kill(pid);
}

// return how many clock tick interrupts have occurred
// since start.
uint64
sys_uptime(void)
{
  uint xticks;

  acquire(&tickslock);
  xticks = ticks;
  release(&tickslock);
  return xticks;
}

uint64
sys_mmap(void)
{
  uint64 vaddr, length, offset;
  int prot, flags, fd;
  struct file *ofile;
  struct proc *p = myproc();
  struct vma *vmalloc = 0;
  uint64 vma_end = VMAEND; // mmap grows downwards from VMAEND(TRAMPFRAME)

  if (argaddr(0, &vaddr) < 0 || argaddr(1, &length) < 0 || argint(2, &prot) || argint(3, &flags) < 0 || argint(4, &fd) < 0 || argaddr(5, &offset) < 0)
    return -1;

  if ((ofile = p->ofile[fd]) == 0)
    return -1;

  // protection check, guarantee the file is readable or writable as the prot declares
  if (prot & PROT_READ)
  {
    if (!ofile->readable)
      return -1;
  }
  if ((prot & PROT_WRITE))
  {
    // MAP_PRIVATE always can write,
    // private means just write to the proc's virtual adress in memory but not write into disk...
    // so others can never see the modification of this file
    if (!(flags & MAP_PRIVATE) && !(ofile->writable))
      return -1;
  }

  // find an empty vma entry and the lowest vadress of current vmas
  // the new vma is set to the lowest virutal adress, this is easy to handle than finding a suitable hole...
  for (int i = 0; i < NVMA; ++i)
  {
    if (p->vmas[i].valid == 0)
    {
      vmalloc = &p->vmas[i];
    }
    else if (p->vmas[i].vaddr < vma_end)
    {
      vma_end = p->vmas[i].vaddr;
    }
  }

  vaddr = PGROUNDDOWN(vma_end - length);

  vmalloc->valid = 1;
  vmalloc->vaddr = vaddr;
  vmalloc->length = length;
  vmalloc->prot = prot;
  vmalloc->flags = flags;
  vmalloc->ofile = ofile;
  vmalloc->offset = offset;

  // increase ofile->nlink, to avoid the freeing inode when proc calls close(),
  // mmap need use ofile anyway, it shouldn't be freed
  filedup(ofile);

  return vaddr;
}

uint64
sys_munmap(void)
{
  uint64 vaddr, length, vabegin, vaend;
  struct vma *vma;

  if (argaddr(0, &vaddr) < 0 || argaddr(1, &length) < 0)
    return -1;

  if ((vma = findvma(vaddr)) == 0)
    return -1;

  if (vaddr > vma->vaddr && vaddr + length < vma->vaddr + vma->length)
  {
    panic("try to dig a hole...which should appears in this lab\n");
  }
  else if (vaddr > vma->vaddr)
  {
    // remove the second half portion
    vabegin = PGROUNDUP(vaddr);
    vaend = PGROUNDUP(vaddr + length);
    vmaunmap(myproc()->pagetable, vabegin, (vaend - vabegin) / PGSIZE, vma);
    vma->length = vabegin - vaddr;
  }
  else if (vaddr + length < vma->vaddr + vma->length)
  {
    // remove the first half portion
    vabegin = PGROUNDDOWN(vaddr);
    vaend = PGROUNDDOWN(vaddr + length);
    vmaunmap(myproc()->pagetable, vabegin, (vaend - vabegin) / PGSIZE, vma);
    vma->vaddr += (vaend - vabegin);
    vma->offset += (vaend - vabegin);
    vma->length -= (vaend - vabegin);
  }
  else
  {
    // remove all
    vabegin = PGROUNDDOWN(vma->vaddr);
    vaend = PGROUNDUP(vma->vaddr + vma->length);
    vmaunmap(myproc()->pagetable, vabegin, (vaend - vabegin) / PGSIZE, vma);

    fileclose(vma->ofile); // close the link by mmap
    memset(vma, 0, sizeof(struct vma));
  }

  return 0;
}