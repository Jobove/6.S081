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
gvma()
{
  struct proc *p = myproc();
  for(int i = 0; i < 16; ++i) {
    if(!p->vma[i].used) {
      return i;
    }
  }

  panic("gvma: no available.");
}

uint64
sys_mmap(void)
{
  uint64 addr;
  int length, prot, flags, fd, offset;
  struct proc *p = myproc();

  if(argaddr(0, &addr) < 0 || argint(1, &length) < 0 || argint(2, &prot) < 0 || argint(3, &flags) < 0 || argint(4, &fd) < 0 || argint(5, &offset) < 0)  return 0xfffffffffffffff;
  if(addr != 0) panic("mmap: address given.");

  if(p->sz + length > TRAPFRAME - PGSIZE) return 0xffffffffffffffff;
  struct file *f = p->ofile[fd];
  if(!f || checkperm(f, prot, flags) < 0) return 0xffffffffffffffff;

  struct vma *v = &p->vma[gvma()];
  v->used = 1;
  v->addr = p->sz;
  v->perm = prot;
  v->flags = flags;
  v->length = length;
  v->file = p->ofile[fd];

  filedup(v->file);
  p->sz += length;
  readintopage(v->file, 1, v->addr, 0, length);

  return v->addr;
}

int
kunmap(uint64 addr, int length)
{
  struct proc *p = myproc();

  struct vma *v = 0;
  for(int i = 0; i < 16; ++i){
    if(!p->vma[i].used)  continue;
    
    if(addr >= p->vma[i].addr && addr < p->vma[i].addr + length){
      v = &p->vma[i];
      break;
    }
  }
  if(!v)  return 0;
  if(v->flags & MAP_SHARED) filewrite(v->file, addr, length);
  uvmunmap(p->pagetable, PGROUNDDOWN(addr), length / PGSIZE, 1);

  if(addr == v->addr && length == v->length){
    v->used = 0;
    addr = 0;
    length = 0;
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
