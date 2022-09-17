#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "spinlock.h"
#include "proc.h"
#include "sysinfo.h"

uint64
sys_info(void)
{
    uint64 addr;
    if (argaddr(0, &addr) < 0)
        return -1;
    struct sysinfo info;
    info.freemem = free_mem();
    info.nproc = proc_num();
    struct proc *p = myproc();
    if (copyout(p->pagetable, addr, (char *)&info, sizeof(info)) < 0)
        return -1;
    return 0;
}