#include "param.h"
#include "types.h"
#include "memlayout.h"
#include "elf.h"
#include "riscv.h"
#include "defs.h"
#include "spinlock.h"
#include "proc.h"
#include "fs.h"

/*
 * the kernel's page table.
 */
pagetable_t kernel_pagetable;

extern char etext[]; // kernel.ld sets this to end of kernel code.

extern char trampoline[]; // trampoline.S

// Make a direct-map page table for the kernel.
pagetable_t
kvmmake(void)
{
  pagetable_t kpgtbl;

  kpgtbl = (pagetable_t)kalloc();
  memset(kpgtbl, 0, PGSIZE);

  // uart registers
  kvmmap(kpgtbl, UART0, UART0, PGSIZE, PTE_R | PTE_W);

  // virtio mmio disk interface - Disk 1
  kvmmap(kpgtbl, VIRTIO0, VIRTIO0, PGSIZE, PTE_R | PTE_W);

  // virtio mmio disk interface - Disk 2
  kvmmap(kpgtbl, VIRTIO1, VIRTIO1, PGSIZE, PTE_R | PTE_W);
  // PLIC
  kvmmap(kpgtbl, PLIC, PLIC, 0x4000000, PTE_R | PTE_W);

  // map kernel text executable and read-only.
  kvmmap(kpgtbl, KERNBASE, KERNBASE, (uint64)etext - KERNBASE, PTE_R | PTE_X);

  // map kernel data and the physical RAM we'll make use of.
  kvmmap(kpgtbl, (uint64)etext, (uint64)etext, PHYSTOP - (uint64)etext,
         PTE_R | PTE_W);

  // map the trampoline for trap entry/exit to
  // the highest virtual address in the kernel.
  kvmmap(kpgtbl, TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);

  // allocate and map a kernel stack for each process.
  proc_mapstacks(kpgtbl);

  return kpgtbl;
}

// add a mapping to the kernel page table.
// only used when booting.
// does not flush TLB or enable paging.
void
kvmmap(pagetable_t kpgtbl, uint64 va, uint64 pa, uint64 sz, int perm)
{
  if (mappages(kpgtbl, va, sz, pa, perm) != 0)
    panic("kvmmap");
}

// Initialize the kernel_pagetable, shared by all CPUs.
void
kvminit(void)
{
  kernel_pagetable = kvmmake();
}

// Switch the current CPU's h/w page table register to
// the kernel's page table, and enable paging.
void
kvminithart()
{
  // wait for any previous writes to the page table memory to finish.
  sfence_vma();

  w_satp(MAKE_SATP(kernel_pagetable));

  // flush stale entries from the TLB.
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

// Allocate a page-table page. Swap I/O is attempted only when no spinlock
// is held, as enforced by swapout_one_internal().
static char *
kalloc_pagetable_page(void)
{
  return kalloc_user_page(0);
}

pte_t *
walk(pagetable_t pagetable, uint64 va, int alloc)
{
  if (va >= MAXVA)
    panic("walk");

  for (int level = 2; level > 0; level--) {
    pte_t *pte = &pagetable[PX(level, va)];
    if (*pte & PTE_V) {
      pagetable = (pagetable_t)PTE2PA(*pte);
    } else {
      if (!alloc || (pagetable = (pagetable_t)kalloc_pagetable_page()) == 0)
        return 0;
      memset(pagetable, 0, PGSIZE);
      *pte = PA2PTE(pagetable) | PTE_V;
    }
  }
  return &pagetable[PX(0, va)];
}

// Look up a virtual address, return the physical address,
// or 0 if not mapped.// Can only be used to look up user pages.
uint64
walkaddr(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;
  uint64 pa;

  if (va >= MAXVA)
    return 0;

  pte = walk(pagetable, va, 0);
  if (pte == 0)
    return 0;
  if ((*pte & PTE_V) == 0)
    return 0;
  if ((*pte & PTE_U) == 0)
    return 0;
  pa = PTE2PA(*pte);
  return pa;
}

// Create PTEs for virtual addresses starting at va that refer to
// physical addresses starting at pa.
// va and size MUST be page-aligned.
// Returns 0 on success, -1 if walk() couldn't
// allocate a needed page-table page.
int
mappages(
  pagetable_t pagetable, uint64 va, uint64 size, uint64 pa,
  int perm) //pa có được khi khi gọi kalloc() trong uvmalloc() hoặc vmfault()
{
  uint64 a, last;
  pte_t *pte;

  if ((va % PGSIZE) != 0)
    panic("mappages: va not aligned");

  if ((size % PGSIZE) != 0)
    panic("mappages: size not aligned");

  if (size == 0)
    panic("mappages: size");

  a = va;
  last = va + size - PGSIZE;
  for (;;) {
    if ((pte = walk(pagetable, a, 1)) == 0)
      return -1;
    if (*pte & PTE_V)
      panic("mappages: remap");
    *pte = PA2PTE(pa) | perm | PTE_V;

    // Register user pages in Reverse Mapping.
    if (perm & PTE_U) {
      if (rmap_register(pa, pagetable, a) != 0) {
        *pte = 0;
        return -1;
      }
    }
    if (a == last)
      break;
    a += PGSIZE;
    pa += PGSIZE;
  }
  return 0;
}

// create an empty user page table.
// returns 0 if out of memory.
pagetable_t
uvmcreate()
{
  pagetable_t pagetable;
  pagetable = (pagetable_t)kalloc_user_page(0);
  if (pagetable == 0)
    return 0;
  memset(pagetable, 0, PGSIZE);
  return pagetable;
}

// Remove npages of mappings starting from va. va must be
// page-aligned. It's OK if the mappings don't exist.
// Optionally free the physical memory.

// Remove npages of mappings starting from va.
// va must be page-aligned.
// Handles both physical pages and swapped pages.

void
uvmunmap(pagetable_t pagetable, uint64 va, uint64 npages, int do_free)
{
  uint64 a;
  pte_t *pte;
  uint64 end;

  if ((va % PGSIZE) != 0)
    panic("uvmunmap: not aligned");

  for (a = va; a < va + npages * PGSIZE; a += PGSIZE) {
    if ((pte = walk(pagetable, a, 0)) == 0)
      continue;

    // Case 1: Page is stored on swap disk.
    if (*pte & PTE_SWAP) {
      if (*pte & PTE_V)
        panic("uvmunmap: valid swap PTE");
      uint slot = PTE_SWAP_SLOT(*pte);

      *pte = 0;
      // Do not make a slot reusable while a PTE still refers to it.
      swap_free(slot);
      continue;
    }

    // Case 2: Page is not mapped to physical memory.
    if ((*pte & PTE_V) == 0)
      continue;

    uint64 pa = PTE2PA(*pte);
    int user = *pte & PTE_U;

    // Remove the mapping before making its frame available for reuse.
    *pte = 0;

    // If physical memory is kept, pin the frame
    // so it cannot be selected for swapping.
    if (!do_free && user)
      rmap_pin(pa);

    // kfree() also unregisters the frame from RMAP.
    if (do_free)
      kfree((void *)pa);
  }

  if (!do_free || npages == 0)
    return;

  end = va + npages * PGSIZE;

  // Release empty lower-level page-table pages left behind by a failed
  // growth rollback or a partial sbrk shrink.
  for (uint64 l2base = va & ~((1L << 30) - 1); l2base < end;
       l2base += 1L << 30) {
    pte_t *l1pte = &pagetable[PX(2, l2base)];
    if ((*l1pte & PTE_V) == 0)
      continue;

    pagetable_t l1 = (pagetable_t)PTE2PA(*l1pte);
    uint64 range_start = va > l2base ? va : l2base;
    uint64 l2end = l2base + (1L << 30);
    uint64 range_end = end < l2end ? end : l2end;
    int first = PX(1, range_start);
    int last = PX(1, range_end - 1);

    for (int i = first; i <= last; i++) {
      pte_t *l0pte = &l1[i];
      if ((*l0pte & PTE_V) == 0)
        continue;

      pagetable_t l0 = (pagetable_t)PTE2PA(*l0pte);
      int empty = 1;
      for (int j = 0; j < 512; j++) {
        if (l0[j] != 0) {
          empty = 0;
          break;
        }
      }

      if (empty) {
        *l0pte = 0;
        kfree((void *)l0);
      }
    }

    int empty = 1;
    for (int i = 0; i < 512; i++) {
      if (l1[i] != 0) {
        empty = 0;
        break;
      }
    }

    if (empty) {
      *l1pte = 0;
      kfree((void *)l1);
    }
  }
}

// Allocate a user page.
// If physical memory is exhausted, swap out one page and retry.
char *
kalloc_user_page(int for_swapin)
{
  char *mem;

  mem = kalloc();

  if (mem == 0) {
    if (swapout_one() == 0)
      mem = kalloc();
    if (mem == 0 && for_swapin && swapout_one_for_swapin() == 0)
      mem = kalloc();
  }

  return mem;
}

// Allocate one zero-filled user page and map it at va.
//
// A new 2MB region also needs a page-table page. That page is allocated
// inside mappages()/walk() and must not itself start swap I/O, because
// walk() can run while a process lock is held. If the user frame took
// the last free page, return it, evict another resident page, and retry
// so both the user frame and the page-table page can be allocated.
// Returns the user physical address, or 0 on failure.
static uint64
alloc_map_user_page(pagetable_t pagetable, uint64 va, int perm)
{
  char *mem;

  for (int attempt = 0; attempt < 4; attempt++) {
    mem = kalloc_user_page(0);
    if (mem == 0)
      return 0;

    memset(mem, 0, PGSIZE);

    if (mappages(pagetable, va, PGSIZE, (uint64)mem, perm) == 0)
      return (uint64)mem;

    kfree(mem);

    if (swapout_one() != 0)
      return 0;
  }

  return 0;
}

// Allocate PTEs and physical memory to grow a process from oldsz to
// newsz, which need not be page aligned.  Returns new size or 0 on error.
uint64
uvmalloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz, int xperm)
{
  char *mem;
  uint64 a;

  if (newsz < oldsz)
    return oldsz;

  oldsz = PGROUNDUP(oldsz);
  for (a = oldsz; a < newsz; a += PGSIZE) {
    mem = (char *)alloc_map_user_page(pagetable, a, PTE_R | PTE_U | xperm);
    if (mem == 0) {
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

  if (PGROUNDUP(newsz) < PGROUNDUP(oldsz)) {
    int npages = (PGROUNDUP(oldsz) - PGROUNDUP(newsz)) / PGSIZE;
    uvmunmap(pagetable, PGROUNDUP(newsz), npages, 1);
  }

  return newsz;
}

// Recursively free page-table pages.
// All leaf mappings must already have been removed.
void
freewalk(pagetable_t pagetable)
{
  // there are 2^9 = 512 PTEs in a page table.
  for (int i = 0; i < 512; i++) {
    pte_t pte = pagetable[i];
    if ((pte & PTE_V) && (pte & (PTE_R | PTE_W | PTE_X)) == 0) {
      // this PTE points to a lower-level page table.
      uint64 child = PTE2PA(pte);
      freewalk((pagetable_t)child);
      pagetable[i] = 0;
    } else if (pte & PTE_V) {
      panic("freewalk: leaf");
    }
  }
  kfree((void *)pagetable);
}

// Free user memory pages,
// then free page-table pages.
void
uvmfree(pagetable_t pagetable, uint64 sz)
{
  if (sz > 0)
    uvmunmap(pagetable, 0, PGROUNDUP(sz) / PGSIZE, 1);

  // All user mappings of this page table must be removed.
  if (rmap_count_pt(pagetable) != 0) {
    panic("uvmfree: rmap leak");
  }

  freewalk(pagetable);
}

// Given a parent process's page table, copy
// its memory into a child's page table.
// Copies both the page table and the
// physical memory.
// returns 0 on success, -1 on failure.
// frees any allocated pages on failure.

int
uvmcopy(pagetable_t old, pagetable_t new, uint64 sz)
{
  pte_t *pte;
  uint64 pa, i;
  uint flags;
  char *mem;

  for (i = 0; i < sz; i += PGSIZE) {
    if ((pte = walk(old, i, 0)) == 0)
      continue;

    // If the page is stored on swap, restore it first.
    if (*pte & PTE_SWAP) {
      if (vmfault(old, sz, i, 1) == 0)
        goto err;
    }

    // Skip pages that have not been allocated.
    if ((*pte & PTE_V) == 0)
      continue;

    pa = PTE2PA(*pte);
    flags = PTE_FLAGS(*pte);

    for (int attempt = 0; attempt < 4; attempt++) {
      if ((mem = kalloc_user_page(0)) == 0)
        goto err;

      memmove(mem, (char *)pa, PGSIZE);

      if (mappages(new, i, PGSIZE, (uint64)mem, flags) == 0)
        break;

      kfree(mem);
      if (swapout_one() != 0 || attempt == 3)
        goto err;
    }
  }

  return 0;

err:
  uvmunmap(new, 0, i / PGSIZE, 1);
  return -1;
}

// mark a PTE invalid for user access.
// used by exec for the user stack guard page.

void
uvmclear(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;

  pte = walk(pagetable, va, 0);

  if (pte == 0)
    panic("uvmclear");

  // Pin the guard page so it cannot be selected for swapping.
  if (*pte & PTE_V)
    rmap_pin(PTE2PA(*pte));
  *pte &= ~PTE_U;
}

// Copy from kernel to user.
// Copy len bytes from src to virtual address dstva in a given page table.
// Return 0 on success, -1 on error.
int
copyout(pagetable_t pagetable, uint64 psz, uint64 dstva, char *src, uint64 len)
{
  uint64 n, va0, pa0;
  pte_t *pte;

  while (len > 0) {
    va0 = PGROUNDDOWN(dstva);
    if (va0 >= MAXVA)
      return -1;

    pa0 = walkaddr(pagetable, va0);
    if (pa0 == 0) {
      if ((pa0 = vmfault(pagetable, psz, va0, 0)) == 0) {
        return -1;
      }
    }

    pte = walk(pagetable, va0, 0);
    // forbid copyout over read-only user text pages.
    if ((*pte & PTE_W) == 0)
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
int
copyin(pagetable_t pagetable, uint64 psz, char *dst, uint64 srcva, uint64 len)
{
  uint64 n, va0, pa0;

  while (len > 0) {
    va0 = PGROUNDDOWN(srcva);
    pa0 = walkaddr(pagetable, va0);
    if (pa0 == 0) {
      if ((pa0 = vmfault(pagetable, psz, va0, 1)) == 0) {
        return -1;
      }
    }
    n = PGSIZE - (srcva - va0);
    if (n > len)
      n = len;
    memmove(dst, (void *)(pa0 + (srcva - va0)), n);

    len -= n;
    dst += n;
    srcva = va0 + PGSIZE;
  }
  return 0;
}

// Copy a null-terminated string from user to kernel.
// Copy bytes to dst from virtual address srcva in a given page table,
// until a '\0', or max.
// Return 0 on success, -1 on error.
int
copyinstr(pagetable_t pagetable, uint64 psz, char *dst, uint64 srcva,
          uint64 max)
{
  uint64 n, va0, pa0;
  int got_null = 0;

  while (got_null == 0 && max > 0) {
    va0 = PGROUNDDOWN(srcva);
    pa0 = walkaddr(pagetable, va0);
    if (pa0 == 0) {
      if ((pa0 = vmfault(pagetable, psz, va0, 1)) == 0) {
        return -1;
      }
    }
    n = PGSIZE - (srcva - va0);
    if (n > max)
      n = max;

    char *p = (char *)(pa0 + (srcva - va0));
    while (n > 0) {
      if (*p == '\0') {
        *dst = '\0';
        got_null = 1;
        break;
      } else {
        *dst = *p;
      }
      --n;
      --max;
      p++;
      dst++;
    }

    srcva = va0 + PGSIZE;
  }
  if (got_null) {
    return 0;
  } else {
    return -1;
  }
}

// allocate and map user memory if process is referencing a page
// that was lazily allocated in sys_sbrk().
// returns 0 if va is invalid or already mapped, or if
// out of physical memory, and physical address if successful.

uint64
vmfault(pagetable_t pagetable, uint64 psz, uint64 va, int read)
{
  uint64 mem;
  pte_t *pte;
  uint slot;
  uint flags;

  if (va >= psz)
    return 0;

  va = PGROUNDDOWN(va);

  if (ismapped(pagetable, va))
    return 0;

  pte = walk(pagetable, va, 0);

  // Case 1: The page is stored on the swap disk.
  if (pte != 0 && (*pte & PTE_SWAP)) {
    if (*pte & PTE_V)
      panic("vmfault: valid swap PTE");
    // Swap I/O may sleep; copy helpers can reach here under spinlocks.
    if (spinlock_depth() != 0)
      return 0;
    slot = PTE_SWAP_SLOT(*pte);
    flags = PTE_FLAGS(*pte);

    // Do not restore pages without user permission.
    if ((flags & PTE_U) == 0)
      return 0;

    // Allocate a physical page.
    mem = (uint64)kalloc_user_page(1);
    if (mem == 0)
      return 0;

    // Read the original page from swap storage.
    swap_in(slot, (void *)mem);

    // Restore the original PTE permissions.
    flags &= ~(PTE_SWAP | PTE_V);
    *pte = PA2PTE(mem) | flags | PTE_V;

    // Keep RMAP registration synchronized with the resident PTE.
    if (rmap_register(mem, pagetable, va) != 0) {
      *pte = PTE_MAKE_SWAP(slot, flags);
      swap_in_abort(slot);
      kfree((void *)mem);
      return 0;
    }

    // The PTE now references RAM, so the slot can be reused.
    swap_in_commit(slot);

    // Record a successfully restored swap page fault.
    swap_page_fault();

    sfence_vma();
    return mem;
  }

  // Case 2: The page has not been allocated yet (lazy allocation).
  return alloc_map_user_page(pagetable, va, PTE_W | PTE_U | PTE_R);
}

// Check whether a virtual address is mapped to physical memory.
int
ismapped(pagetable_t pagetable, uint64 va)
{
  pte_t *pte = walk(pagetable, va, 0);

  if (pte == 0)
    return 0;

  if (*pte & PTE_V)
    return 1;

  return 0;
}

// Swap out one user page using FIFO replacement.
// Return 0 on success, -1 on failure.
static int
swapout_one_internal(int emergency)
{
  uint64 pa;
  uint64 va;
  uint64 sequence;
  uint64 after = 0;
  pagetable_t pt;
  pagetable_t skip_pt = 0;
  pte_t *pte;
  uint slot;
  uint flags;
  int allocated_slot;

  // Never start blocking swap I/O while the caller holds spinlocks.
  if (spinlock_depth() != 0)
    return -1;

  // Wait for in-flight swap operations when every candidate is temporarily
  // reserved. Skip other frames of an unreservable page table per pass.
  for (;;) {
    int pass_busy = 0;

    after = 0;
    skip_pt = 0;

    // Try eligible FIFO frames in increasing load_time order.
    for (;;) {
      if (rmap_select_victim_after(&pa, &pt, &va, &sequence, after, skip_pt) <
          0) {
        break;
      }
      // Advance the cursor even if this candidate cannot be reserved.
      after = sequence;

      // A RUNNING process or temporary exec page table is not eligible.
      int reserve_result = proc_vm_reserve(pt);
      if (reserve_result < 0) {
        if (reserve_result == -2)
          pass_busy++;
        skip_pt = pt;
        continue;
      }

      // Claim the frame only after reserving its owning process. This
      // prevents page-table teardown from freeing a selected busy frame.
      if (rmap_reserve_victim(pa, pt, va, sequence) < 0) {
        if (proc_vm_release(pt) < 0)
          panic("swapout: release process after stale victim");
        continue;
      }

      // The process cannot run while vm_busy is set.
      pte = walk(pt, va, 0);

      if (pte == 0 || (*pte & PTE_V) == 0 || (*pte & PTE_U) == 0 ||
          PTE2PA(*pte) != pa) {
        if (rmap_release_victim(pa) < 0)
          panic("swapout: release invalid victim");
        if (proc_vm_release(pt) < 0)
          panic("swapout: release process reservation");
        continue;
      }

      // Reserve a swap slot before performing disk I/O.
      allocated_slot = emergency ? swap_alloc_emergency() : swap_alloc();

      if (allocated_slot < 0) {
        if (rmap_release_victim(pa) < 0)
          panic("swapout: release victim without slot");
        if (proc_vm_release(pt) < 0)
          panic("swapout: release process without slot");
        return -1;
      }

      slot = (uint)allocated_slot;
      flags = PTE_FLAGS(*pte);

      // No spinlock or process lock is held during disk I/O.
      swap_out(slot, (void *)pa, proc_vm_pid(pt), va);

      // Reservation should keep this mapping stable. Revalidate anyway.
      if ((*pte & PTE_V) == 0 || (*pte & PTE_U) == 0 || PTE2PA(*pte) != pa) {
        swap_free(slot);

        if (rmap_release_victim(pa) < 0)
          panic("swapout: release changed victim");
        if (proc_vm_release(pt) < 0)
          panic("swapout: release changed process");
        return -1;
      }

      // Commit the swap PTE before returning the frame to the allocator.
      *pte = PTE_MAKE_SWAP(slot, flags);
      sfence_vma();

      // This consumes the RMAP busy reservation atomically.
      kfree_victim((void *)pa);

      if (proc_vm_release(pt) < 0)
        panic("swapout: release committed process");

      return 0;
    }

    if (pass_busy == 0 || myproc() == 0)
      break;
    proc_vm_wait();
  }

  return -1;
}

int
swapout_one(void)
{
  return swapout_one_internal(0);
}

int
swapout_one_for_swapin(void)
{
  return swapout_one_internal(1);
}
