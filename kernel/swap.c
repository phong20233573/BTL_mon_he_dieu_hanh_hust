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

  swap_stats.current_policy = POLICY_FIFO;
  swap_stats.page_faults = 0;
  swap_stats.swap_out_count = 0;
  swap_stats.swap_in_count = 0;
  swap_stats.used_slots = 0;

  release(&swap_lock);

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
      swap_stats.used_slots++;

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

  if (swap_stats.used_slots == 0) {
    release(&swap_lock);
    panic("swap_free: invalid used_slots");
  }

  swap_stats.used_slots--;

  release(&swap_lock);
}

// Write one memory page to swap disk
void
swap_out(uint slot, void *page)
{
  if (slot >= SWAP_SLOTS || page == 0)
    panic("swap_out: invalid argument");

  virtio_swap_rw(page, slot, 1);

  acquire(&swap_lock);
  swap_stats.swap_out_count++;
  release(&swap_lock);
}

// Read one memory page from swap disk
void
swap_in(uint slot, void *page)
{
  if (slot >= SWAP_SLOTS || page == 0)
    panic("swap_in: invalid argument");

  virtio_swap_rw(page, slot, 0);

  acquire(&swap_lock);
  swap_stats.swap_in_count++;
  release(&swap_lock);
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
