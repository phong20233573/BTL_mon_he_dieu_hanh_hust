#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "spinlock.h"

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
  release(&swap_lock);

  printk("Swap initialized: %d slots, %d bytes bitmap\n",
         SWAP_SLOTS, SWAP_BITMAP_SIZE);
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
}

// Read one memory page from swap disk
void
swap_in(uint slot, void *page)
{
  if (slot >= SWAP_SLOTS || page == 0)
    panic("swap_in: invalid argument");

  virtio_swap_rw(page, slot, 0);
}
