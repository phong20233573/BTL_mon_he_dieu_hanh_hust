#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "spinlock.h"
#include "swap.h"

// Swap disk: 64 MB
// Page size: 4096 bytes
// Number of slots: 64 MB / 4096 = 16384
#define SWAP_DISK_SIZE   (64 * 1024 * 1024)
#define SWAP_SLOTS       (SWAP_DISK_SIZE / PGSIZE)
#define SWAP_BITMAP_SIZE ((SWAP_SLOTS + 7) / 8)

// Bitmap: 1 bit represents 1 swap slot
static uchar swap_bitmap[SWAP_BITMAP_SIZE];

// Lock protects bitmap operations
static struct spinlock swap_lock;
static struct spinlock swap_stats_lock;
static int current_policy = POLICY_FIFO;
static uint64 page_faults;
static uint64 swap_out_count;
static uint64 swap_in_count;

struct swap_page {
  pagetable_t pagetable;
  uint64 va;
  int valid;
};

static struct swap_page swap_pages[SWAP_SLOTS];

// Check whether a slot is allocated
static int
swap_test_bit(uint slot)
{
  return (swap_bitmap[slot / 8] >> (slot % 8)) & 1;
}

void
swap_set_policy(int policy)
{
  current_policy = policy;
}

int
swap_get_policy(void)
{
  return current_policy;
}

void
swap_reset_counters(void)
{
  acquire(&swap_stats_lock);
  page_faults = 0;
  swap_out_count = 0;
  swap_in_count = 0;
  release(&swap_stats_lock);
}

void
swap_record_page_fault(void)
{
  acquire(&swap_stats_lock);
  page_faults++;
  release(&swap_stats_lock);
}

void
swap_get_stats(struct swap_stat *st)
{
  uint64 used_slots = 0;

  acquire(&swap_lock);
  for (uint slot = 0; slot < SWAP_SLOTS; slot++)
    if (swap_test_bit(slot))
      used_slots++;
  release(&swap_lock);

  acquire(&swap_stats_lock);
  st->current_policy = current_policy;
  st->page_faults = page_faults;
  st->swap_out_count = swap_out_count;
  st->swap_in_count = swap_in_count;
  st->used_slots = used_slots;
  release(&swap_stats_lock);
}

int
swap_page_record(uint slot, pagetable_t pagetable, uint64 va)
{
  if (slot >= SWAP_SLOTS || pagetable == 0 || va % PGSIZE != 0)
    return -1;

  acquire(&swap_lock);
  swap_pages[slot].pagetable = pagetable;
  swap_pages[slot].va = va;
  swap_pages[slot].valid = 1;
  release(&swap_lock);
  return 0;
}

int
swap_page_matches(uint slot, pagetable_t pagetable, uint64 va)
{
  int matches = 0;

  if (slot >= SWAP_SLOTS)
    return 0;

  acquire(&swap_lock);
  matches = swap_pages[slot].valid &&
            swap_pages[slot].pagetable == pagetable &&
            swap_pages[slot].va == va;
  release(&swap_lock);
  return matches;
}

void
swap_page_remove(uint slot)
{
  if (slot >= SWAP_SLOTS)
    panic("swap_page_remove: invalid slot");

  acquire(&swap_lock);
  swap_pages[slot].pagetable = 0;
  swap_pages[slot].va = 0;
  swap_pages[slot].valid = 0;
  release(&swap_lock);
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
  initlock(&swap_stats_lock, "swap_stats");

  acquire(&swap_lock);
  memset(swap_bitmap, 0, sizeof(swap_bitmap));
  memset(swap_pages, 0, sizeof(swap_pages));
  release(&swap_lock);

  current_policy = POLICY_FIFO;
  swap_reset_counters();

  printk("Swap initialized: %d slots, %d bytes bitmap\n", SWAP_SLOTS,
         SWAP_BITMAP_SIZE);
}

// Allocate one free swap slot
// Return slot number, or -1 if no slot is available
int
swap_alloc(void)
{
  acquire(&swap_lock);

  for (uint slot = 0; slot < SWAP_SLOTS; slot++) {
    if (!swap_test_bit(slot)) {
      swap_set_bit(slot);
      release(&swap_lock);
      return slot;
    }
  }

  release(&swap_lock);
  return -1;
}

// Free a previously allocated swap slot
void
swap_free(uint slot)
{
  if (slot >= SWAP_SLOTS)
    panic("swap_free: invalid slot");

  acquire(&swap_lock);

  if (!swap_test_bit(slot)) {
    release(&swap_lock);
    panic("swap_free: slot not allocated");
  }

  swap_clear_bit(slot);
  release(&swap_lock);
}

// Write one memory page to swap disk
void
swap_out(uint slot, void *page)
{
  if (slot >= SWAP_SLOTS || page == 0)
    panic("swap_out: invalid argument");

  virtio_swap_rw(page, slot, 1);

  acquire(&swap_stats_lock);
  swap_out_count++;
  release(&swap_stats_lock);
}

// Read one memory page from swap disk
void
swap_in(uint slot, void *page)
{
  if (slot >= SWAP_SLOTS || page == 0)
    panic("swap_in: invalid argument");

  virtio_swap_rw(page, slot, 0);

  acquire(&swap_stats_lock);
  swap_in_count++;
  release(&swap_stats_lock);
}
