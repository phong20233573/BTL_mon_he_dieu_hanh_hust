#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"
#include "vm.h"
#include "swap.h"
uint64
sys_exit(void)
{
  int n;
  argint(0, &n);
  kexit(n);
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
  return kfork();
}

uint64
sys_wait(void)
{
  uint64 p;
  argaddr(0, &p);
  return kwait(p);
}

uint64
sys_sbrk(void)
{
  uint64 addr;
  int t;
  int n;

  argint(0, &n);
  argint(1, &t);
  addr = myproc()->sz;

  if (t == SBRK_EAGER || n < 0) {
    if (growproc(n) < 0) {
      return -1;
    }
  } else {
    // Lazily allocate memory for this process: increase its memory
    // size but don't allocate memory. If the processes uses the
    // memory, vmfault() will allocate it.
    if (addr + n < addr)
      return -1;
    if (addr + n > TRAPFRAME)
      return -1;
    myproc()->sz += n;
  }
  return addr;
}

uint64
sys_pause(void)
{
  int n;
  uint ticks0;

  argint(0, &n);
  if (n < 0)
    n = 0;
  acquire(&tickslock);
  ticks0 = ticks;
  while (ticks - ticks0 < n) {
    if (killed(myproc())) {
      release(&tickslock);
      return -1;
    }
    sleep_prepare(&ticks);
    release(&tickslock);
    sleep();
    acquire(&tickslock);
  }
  release(&tickslock);
  return 0;
}

uint64
sys_kill(void)
{
  int pid;

  argint(0, &pid);
  return kkill(pid);
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
// Biến giả lập (Mock) dùng cho kiểm thử Tuần 1.
// Sang Tuần 2-3 sẽ nối với biến thật của nhóm ở kalloc.c và trap.c.
static struct swap_stat mock_kstat = {
  .current_policy = POLICY_FIFO,
  .page_faults = 10,
  .swap_out_count = 5,
  .swap_in_count = 3,
  .used_slots = 2
};

uint64
sys_swap_policy(void)
{
  int policy;
  argint(0, &policy);

  // Kiểm tra miền giá trị hợp lệ (0: FIFO, 1: CLOCK, 2: AGING, 3: WORKSET)
  if(policy < POLICY_FIFO || policy > POLICY_WORKSET)
    return -1;

  mock_kstat.current_policy = policy;
  // Giả lập việc reset các bộ đếm khi đổi thuật toán
  mock_kstat.page_faults = 0;
  mock_kstat.swap_out_count = 0;
  mock_kstat.swap_in_count = 0;
  return 0;
}

uint64
sys_swap_stats(void)
{
  uint64 user_addr;
  struct proc *p = myproc();

  argaddr(0, &user_addr);

  // Chốt chặn 1: Từ chối con trỏ NULL (0x0)
  if(user_addr == 0)
    return -1;

  // Chốt chặn 2: Dùng copyout kiểm tra quyền ghi (PTE_W), quyền User (PTE_U)
  // và chép dữ liệu từ Kernel sang User
  if(copyout(p->pagetable,p->sz, user_addr, (char *)&mock_kstat, sizeof(mock_kstat)) < 0)
    return -1;

  return 0;
}