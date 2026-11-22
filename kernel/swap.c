#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "spinlock.h"
#include "swap.h"

// Swap disk: 64 MB
// Page size: 4096 bytes
// Number of slots: 64 MB / 4096 = 16384
#define SWAP_DISK_SIZE      (64 * 1024 * 1024)
#define SWAP_SLOTS          (SWAP_DISK_SIZE / PGSIZE)
#define SWAP_RESERVED_SLOTS 1
#define SWAP_BITMAP_SIZE    ((SWAP_SLOTS + 7) / 8)

enum swap_slot_state {
  SLOT_FREE,
  SLOT_RESERVED,
  SLOT_WRITING,
  SLOT_VALID,
  SLOT_READING,
  SLOT_READ_DONE,
};

// Bitmap: 1 bit represents 1 swap slot
static uchar swap_bitmap[SWAP_BITMAP_SIZE];
static uchar swap_slot_state[SWAP_SLOTS];
static int swap_slot_pid[SWAP_SLOTS];
static uint64 swap_slot_va[SWAP_SLOTS];

// Lock protects bitmap operations
static struct spinlock swap_lock;

// Thống kê hoạt động Swap thực tế.
static struct swap_stat swap_stats = {.current_policy = POLICY_FIFO,
                                      .page_faults = 0,
                                      .swap_out_count = 0,
                                      .swap_in_count = 0,
                                      .used_slots = 0};

// Check whether a slot is allocated
static int
swap_test_bit(uint slot)
{
  return (swap_bitmap[slot / 8] >> (slot % 8)) & 1;
}

// Mark a slot as allocated
static void
swap_set_bit(uint slot)
{
  swap_bitmap[slot / 8] |= (1 << (slot % 8));
}

// Mark a slot as free
static void
swap_clear_bit(uint slot)
{
  swap_bitmap[slot / 8] &= ~(1 << (slot % 8));
}

// Initialize swap bitmap
void
swap_init(void)
{
  initlock(&swap_lock, "swap");

  acquire(&swap_lock);
  memset(swap_bitmap, 0, sizeof(swap_bitmap));
  memset(swap_slot_state, SLOT_FREE, sizeof(swap_slot_state));
  memset(swap_slot_pid, 0xff, sizeof(swap_slot_pid));
  memset(swap_slot_va, 0, sizeof(swap_slot_va));

  swap_stats.current_policy = POLICY_FIFO;
  swap_stats.page_faults = 0;
  swap_stats.swap_out_count = 0;
  swap_stats.swap_in_count = 0;
  swap_stats.used_slots = 0;

  release(&swap_lock);

  printk("Swap initialized: %d slots, %d bytes bitmap\n", SWAP_SLOTS,
         SWAP_BITMAP_SIZE);
}

static int
swap_alloc_internal(int emergency)
{
  acquire(&swap_lock);

  if (!emergency && swap_stats.used_slots >= SWAP_SLOTS - SWAP_RESERVED_SLOTS) {
    release(&swap_lock);
    return -1;
  }

  for (uint slot = 0; slot < SWAP_SLOTS; slot++) {
    if (!swap_test_bit(slot)) {
      if (swap_slot_state[slot] != SLOT_FREE) {
        release(&swap_lock);
        panic("swap_alloc: free bitmap slot has state");
      }
      if (swap_stats.used_slots >= SWAP_SLOTS) {
        release(&swap_lock);
        panic("swap_alloc: used_slots overflow");
      }
      swap_set_bit(slot);
      swap_slot_state[slot] = SLOT_RESERVED;
      swap_slot_pid[slot] = -1;
      swap_slot_va[slot] = 0;
      swap_stats.used_slots++;

      release(&swap_lock);
      return slot;
    }

    if (swap_slot_state[slot] == SLOT_FREE) {
      release(&swap_lock);
      panic("swap_alloc: allocated bitmap slot is free");
    }
  }

  release(&swap_lock);
  return -1;
}

// Keep one slot free so a swapped page can still evict a resident page
// when normal swap capacity is exhausted.
int
swap_alloc(void)
{
  return swap_alloc_internal(0);
}

// Only the page-in path may consume the recovery slot temporarily.
int
swap_alloc_emergency(void)
{
  return swap_alloc_internal(1);
}

// Free a previously allocated swap slot
void
swap_free(uint slot)
{
  if (slot >= SWAP_SLOTS)
    panic("swap_free: invalid slot");

  acquire(&swap_lock);

  // Slot must currently be allocated.
  if (!swap_test_bit(slot)) {
    release(&swap_lock);
    panic("swap_free: slot not allocated");
  }

  if (swap_slot_state[slot] != SLOT_VALID) {
    release(&swap_lock);
    panic("swap_free: slot not valid");
  }

  // Validate the counter before modifying the bitmap.
  if (swap_stats.used_slots == 0) {
    release(&swap_lock);
    panic("swap_free: invalid used_slots");
  }

  // Release the slot and update the counter atomically.
  swap_clear_bit(slot);
  swap_slot_state[slot] = SLOT_FREE;
  swap_slot_pid[slot] = -1;
  swap_slot_va[slot] = 0;
  swap_stats.used_slots--;

  release(&swap_lock);
}

static void
swap_write_begin(uint slot)
{
  if (slot >= SWAP_SLOTS)
    panic("swap_write_begin: invalid slot");

  acquire(&swap_lock);
  if (!swap_test_bit(slot) || swap_slot_state[slot] != SLOT_RESERVED) {
    release(&swap_lock);
    panic("swap_write_begin: invalid slot state");
  }
  swap_slot_state[slot] = SLOT_WRITING;
  release(&swap_lock);
}

static void
swap_write_end(uint slot)
{
  acquire(&swap_lock);
  if (!swap_test_bit(slot) || swap_slot_state[slot] != SLOT_WRITING) {
    release(&swap_lock);
    panic("swap_write_end: invalid slot state");
  }
  swap_slot_state[slot] = SLOT_VALID;
  swap_stats.swap_out_count++;
  release(&swap_lock);
}

static void
swap_read_begin(uint slot)
{
  if (slot >= SWAP_SLOTS)
    panic("swap_read_begin: invalid slot");

  acquire(&swap_lock);
  if (!swap_test_bit(slot) || swap_slot_state[slot] != SLOT_VALID) {
    release(&swap_lock);
    panic("swap_read_begin: invalid slot state");
  }
  swap_slot_state[slot] = SLOT_READING;
  release(&swap_lock);
}

static void
swap_read_end(uint slot)
{
  acquire(&swap_lock);
  if (!swap_test_bit(slot) || swap_slot_state[slot] != SLOT_READING) {
    release(&swap_lock);
    panic("swap_read_end: invalid slot state");
  }
  swap_slot_state[slot] = SLOT_READ_DONE;
  swap_stats.swap_in_count++;
  release(&swap_lock);
}

// Commit a completed read after the page-table entry points to RAM.
void
swap_in_commit(uint slot)
{
  if (slot >= SWAP_SLOTS)
    panic("swap_in_commit: invalid slot");

  acquire(&swap_lock);
  if (!swap_test_bit(slot) || swap_slot_state[slot] != SLOT_READ_DONE ||
      swap_stats.used_slots == 0) {
    release(&swap_lock);
    panic("swap_in_commit: invalid slot state");
  }

  swap_clear_bit(slot);
  swap_slot_state[slot] = SLOT_FREE;
  swap_slot_pid[slot] = -1;
  swap_slot_va[slot] = 0;
  swap_stats.used_slots--;
  release(&swap_lock);
}

// Restore the slot to its readable state if installing the page failed.
void
swap_in_abort(uint slot)
{
  if (slot >= SWAP_SLOTS)
    panic("swap_in_abort: invalid slot");

  acquire(&swap_lock);
  if (!swap_test_bit(slot) || swap_slot_state[slot] != SLOT_READ_DONE) {
    release(&swap_lock);
    panic("swap_in_abort: invalid slot state");
  }
  swap_slot_state[slot] = SLOT_VALID;
  release(&swap_lock);
}

// Write one memory page to swap disk.
void
swap_out(uint slot, void *page, int pid, uint64 va)
{
  if (slot >= SWAP_SLOTS || page == 0)
    panic("swap_out: invalid argument");

  swap_write_begin(slot);
  acquire(&swap_lock);
  swap_slot_pid[slot] = pid;
  swap_slot_va[slot] = va;
  release(&swap_lock);
  virtio_swap_rw(page, slot, 1);
  swap_write_end(slot);
}

// Read one memory page from swap disk.
void
swap_in(uint slot, void *page)
{
  if (slot >= SWAP_SLOTS || page == 0)
    panic("swap_in: invalid argument");

  swap_read_begin(slot);
  virtio_swap_rw(page, slot, 0);
  swap_read_end(slot);
}

// Copy thống kê Swap thực tế.
void
swap_get_stats(struct swap_stat *stats)
{
  if (stats == 0)
    return;

  acquire(&swap_lock);
  memmove(stats, &swap_stats, sizeof(*stats));
  release(&swap_lock);
}

// Hiện tại chỉ hỗ trợ FIFO.
int
swap_set_policy(int policy)
{
  if (policy != POLICY_FIFO)
    return -1;

  acquire(&swap_lock);
  swap_stats.current_policy = policy;
  release(&swap_lock);

  return 0;
}

// Ghi nhận Page Fault khi truy cập trang đang nằm trên Swap.
void
swap_page_fault(void)
{
  acquire(&swap_lock);
  swap_stats.page_faults++;
  release(&swap_lock);
}

// Print the current owner metadata for every allocated swap slot.
void
swap_dump_owners(void)
{
  for (uint slot = 0; slot < SWAP_SLOTS; slot++) {
    int allocated;
    int pid;
    int state;
    uint64 va;

    acquire(&swap_lock);
    allocated = swap_test_bit(slot);
    pid = swap_slot_pid[slot];
    va = swap_slot_va[slot];
    state = swap_slot_state[slot];
    release(&swap_lock);

    if (allocated)
      printk("SWAP OWNER slot=%d pid=%d va=%p state=%d\n", slot, pid,
             (void *)va, state);
  }
}
