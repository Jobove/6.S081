#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "date.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"
#include "fcntl.h"

uint64
sys_exit(void)
{
  int n;
  if(argint(0, &n) < 0)
    return -1;
  exit(n);
  return 0;  // not reached
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
  if(argaddr(0, &p) < 0)
    return -1;
  return wait(p);
}

uint64
sys_sbrk(void)
{
  int addr;
  int n;

  if(argint(0, &n) < 0)
    return -1;
  addr = myproc()->sz;
  if(growproc(n) < 0)
    return -1;
  return addr;
}

uint64
sys_sleep(void)
{
  int n;
  uint ticks0;

  if(argint(0, &n) < 0)
    return -1;
  acquire(&tickslock);
  ticks0 = ticks;
  while(ticks - ticks0 < n){
    if(myproc()->killed){
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

  if(argint(0, &pid) < 0)
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

int
getvma()
{
  struct proc *p = myproc();
  for(int i = 0; i < NVMA; ++i) {
    if(!p->vma[i].used) {
      return i;
    }
  }

  panic("getvma: no available.");
}

uint64
sys_mmap(void)
{
  uint64 addr;
  int length, prot, flags, fd, offset;
  struct proc *p;
  struct file *f;

  if(argaddr(0, &addr) < 0 || argint(1, &length) < 0 || argint(2, &prot) < 0 || argint(3, &flags) < 0 || argint(4, &fd) < 0 || argint(5, &offset) < 0)  return 0xfffffffffffffff;
  if(addr != 0) panic("mmap: address given.");

  p = myproc();
  f = p->ofile[fd];

  if(p->sz + length >= TRAPFRAME) return 0xffffffffffffffff;
  if(!f || checkperm(f, prot, flags) < 0) return 0xffffffffffffffff;

  struct vma *vma = &p->vma[getvma()];

  vma->used = 1;
  vma->addr = p->sz;
  vma->perm = prot;
  vma->flags = flags;
  vma->length = length;
  vma->file = p->ofile[fd];

  filedup(vma->file);
  p->sz += length;
  readintopage(vma->file, 1, vma->addr, 0, length);

  return vma->addr;
}

int
kunmap(uint64 addr, int length)
{
  struct proc *p = myproc();
  struct vma *v = 0;

  for(int i = 0; i < NVMA; ++i){
    if(!p->vma[i].used)  continue;
    if(addr < p->vma[i].addr || addr >= p->vma[i].addr + length)  continue;

    v = &p->vma[i];
  }

  if(!v)  return 0;
  if(v->flags & MAP_SHARED) filewrite(v->file, addr, length);
  uvmunmap(p->pagetable, PGROUNDDOWN(addr), length / PGSIZE, 1);

  if(addr == v->addr && length == v->length){
    v->used = 0;
    v->addr = 0;
    v->length = 0;
    fileclose(v->file);
  } else if(addr + length == v->addr + length){
    v->length -= length;
  } else if(addr == v->addr){
    v->addr += length;
  } else {
    panic("munmap");
  }

  return 0;
}

uint64
sys_munmap(void)
{
  uint64 addr;
  int length;

  if(argaddr(0, &addr) < 0 || argint(1, &length) < 0) return -1;

  return kunmap(addr, length);
}
