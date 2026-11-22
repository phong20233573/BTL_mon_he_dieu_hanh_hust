// Driver for two VirtIO block devices:
// Device 0: fs.img
// Device 1: swap.img

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "buf.h"
#include "virtio.h"

#define NDISK                2
#define SECTOR_SIZE          512
#define SECTORS_PER_PAGE     (PGSIZE / SECTOR_SIZE)
#define SWAP_DISK_SIZE_BYTES (64ULL * 1024 * 1024)
#define SWAP_SLOT_COUNT      (SWAP_DISK_SIZE_BYTES / PGSIZE)

// Access MMIO registers of a specific device.
#define R(d, r) ((volatile uint32 *)((d)->mmio + (r)))

struct disk {
  uint64 mmio;
  uint64 capacity_sectors;

  struct virtq_desc *desc;
  struct virtq_avail *avail;
  struct virtq_used *used;

  char free[NUM];
  uint16 used_idx;

  struct {
    struct buf *b;
    volatile int *done;
    char status;
  } info[NUM];

  struct virtio_blk_req ops[NUM];

  struct spinlock vdisk_lock;
};

// Read the 64-bit capacity from VirtIO Block configuration.
// Capacity is expressed in 512-byte sectors.
static uint64
virtio_read_capacity(struct disk *d)
{
  uint32 generation;
  uint32 generation_after;
  uint32 low;
  uint32 high;

  do {
    generation = *R(d, VIRTIO_MMIO_CONFIG_GENERATION);

    low = *R(d, VIRTIO_MMIO_CONFIG + VIRTIO_BLK_CONFIG_CAPACITY);
    high = *R(d, VIRTIO_MMIO_CONFIG + VIRTIO_BLK_CONFIG_CAPACITY + 4);

    generation_after = *R(d, VIRTIO_MMIO_CONFIG_GENERATION);
  } while (generation != generation_after);

  return ((uint64)high << 32) | low;
}

static struct disk disks[NDISK];

// Initialize one VirtIO block device.
static void
virtio_disk_init_one(struct disk *d, uint64 mmio, char *name)
{
  uint32 status = 0;

  d->mmio = mmio;

  initlock(&d->vdisk_lock, name);

  if (*R(d, VIRTIO_MMIO_MAGIC_VALUE) != 0x74726976 ||
      *R(d, VIRTIO_MMIO_VERSION) != 2 || *R(d, VIRTIO_MMIO_DEVICE_ID) != 2 ||
      *R(d, VIRTIO_MMIO_VENDOR_ID) != 0x554d4551) {
    panic("could not find virtio disk");
  }

  // Reset device.
  *R(d, VIRTIO_MMIO_STATUS) = status;

  // ACKNOWLEDGE.
  status |= VIRTIO_CONFIG_S_ACKNOWLEDGE;
  *R(d, VIRTIO_MMIO_STATUS) = status;

  // DRIVER.
  status |= VIRTIO_CONFIG_S_DRIVER;
  *R(d, VIRTIO_MMIO_STATUS) = status;

  // Negotiate features.
  uint32 features = *R(d, VIRTIO_MMIO_DEVICE_FEATURES);

  features &= ~(1 << VIRTIO_BLK_F_RO);
  features &= ~(1 << VIRTIO_BLK_F_SCSI);
  features &= ~(1 << VIRTIO_BLK_F_FLUSH);
  features &= ~(1 << VIRTIO_BLK_F_CONFIG_WCE);
  features &= ~(1 << VIRTIO_BLK_F_MQ);
  features &= ~(1 << VIRTIO_F_ANY_LAYOUT);
  features &= ~(1 << VIRTIO_RING_F_EVENT_IDX);
  features &= ~(1 << VIRTIO_RING_F_INDIRECT_DESC);

  *R(d, VIRTIO_MMIO_DRIVER_FEATURES) = features;

  status |= VIRTIO_CONFIG_S_FEATURES_OK;
  *R(d, VIRTIO_MMIO_STATUS) = status;

  //Verify that the device accepted our negotiated features.
  status = *R(d, VIRTIO_MMIO_STATUS);

  if (!(status & VIRTIO_CONFIG_S_FEATURES_OK))
    panic("virtio FEATURES_OK unset");

  // Read actual device capacity.
  d->capacity_sectors = virtio_read_capacity(d);

  if (d->capacity_sectors == 0)
    panic("virtio disk has zero capacity");

  printk("VirtIO capacity: %d sectors\n", (int)d->capacity_sectors);

  // Initialize queue 0.
  *R(d, VIRTIO_MMIO_QUEUE_SEL) = 0;

  if (*R(d, VIRTIO_MMIO_QUEUE_READY))
    panic("virtio queue should not be ready");

  uint32 max = *R(d, VIRTIO_MMIO_QUEUE_NUM_MAX);

  if (max == 0)
    panic("virtio disk has no queue");

  if (max < NUM)
    panic("virtio disk queue too short");

  // Allocate queue memory.
  d->desc = kalloc();
  d->avail = kalloc();
  d->used = kalloc();

  if (!d->desc || !d->avail || !d->used)
    panic("virtio disk kalloc");

  memset(d->desc, 0, PGSIZE);
  memset(d->avail, 0, PGSIZE);
  memset(d->used, 0, PGSIZE);

  *R(d, VIRTIO_MMIO_QUEUE_NUM) = NUM;

  *R(d, VIRTIO_MMIO_QUEUE_DESC_LOW) = (uint64)d->desc;

  *R(d, VIRTIO_MMIO_QUEUE_DESC_HIGH) = (uint64)d->desc >> 32;

  *R(d, VIRTIO_MMIO_DRIVER_DESC_LOW) = (uint64)d->avail;

  *R(d, VIRTIO_MMIO_DRIVER_DESC_HIGH) = (uint64)d->avail >> 32;

  *R(d, VIRTIO_MMIO_DEVICE_DESC_LOW) = (uint64)d->used;

  *R(d, VIRTIO_MMIO_DEVICE_DESC_HIGH) = (uint64)d->used >> 32;

  *R(d, VIRTIO_MMIO_QUEUE_READY) = 1;

  for (int i = 0; i < NUM; i++)
    d->free[i] = 1;

  status |= VIRTIO_CONFIG_S_DRIVER_OK;
  *R(d, VIRTIO_MMIO_STATUS) = status;
}

// Initialize both devices.
void
virtio_disk_init(void)
{
  virtio_disk_init_one(&disks[0], VIRTIO0, "virtio_disk0");
  virtio_disk_init_one(&disks[1], VIRTIO1, "virtio_disk1");

  if (disks[1].capacity_sectors < SWAP_SLOT_COUNT * SECTORS_PER_PAGE)
    panic("swap disk capacity too small");

  printk("VirtIO disk 0 initialized: fs.img\n");
  printk("VirtIO disk 1 initialized: swap.img\n");
}

// Allocate one descriptor.
static int
alloc_desc(struct disk *d)
{
  for (int i = 0; i < NUM; i++) {
    if (d->free[i]) {
      d->free[i] = 0;
      return i;
    }
  }

  return -1;
}

// Free one descriptor.
static void
free_desc(struct disk *d, int i)
{
  if (i >= NUM)
    panic("free_desc 1");

  if (d->free[i])
    panic("free_desc 2");

  d->desc[i].addr = 0;
  d->desc[i].len = 0;
  d->desc[i].flags = 0;
  d->desc[i].next = 0;

  d->free[i] = 1;

  wakeup(&d->free[0]);
}

// Free a descriptor chain.
static void
free_chain(struct disk *d, int i)
{
  while (1) {
    int flags = d->desc[i].flags;
    int next = d->desc[i].next;

    free_desc(d, i);

    if (flags & VRING_DESC_F_NEXT)
      i = next;
    else
      break;
  }
}

// Allocate three descriptors.
static int
alloc3_desc(struct disk *d, int *idx)
{
  for (int i = 0; i < 3; i++) {
    idx[i] = alloc_desc(d);

    if (idx[i] < 0) {
      for (int j = 0; j < i; j++)
        free_desc(d, idx[j]);

      return -1;
    }
  }

  return 0;
}

// Generic block-device read/write operation.
static void
virtio_rw(struct disk *d, void *data, uint len, uint64 sector, int write)
{
  uint64 sectors;

  if (data == 0 || len == 0 || len % SECTOR_SIZE != 0)
    panic("virtio_rw: invalid buffer or length");

  sectors = len / SECTOR_SIZE;

  if (sector > d->capacity_sectors || sectors > d->capacity_sectors - sector)
    panic("virtio_rw: request out of range");

  volatile int done = 0;

  acquire(&d->vdisk_lock);

  // Allocate descriptors.
  int idx[3];

  while (alloc3_desc(d, idx) < 0) {
    proc_vm_io_wait(1);

    sleep_prepare(&d->free[0]);
    release(&d->vdisk_lock);

    sleep();

    acquire(&d->vdisk_lock);
    proc_vm_io_wait(0);
  }

  // Descriptor 0: request header.
  struct virtio_blk_req *req = &d->ops[idx[0]];

  req->type = write ? VIRTIO_BLK_T_OUT : VIRTIO_BLK_T_IN;
  req->reserved = 0;
  req->sector = sector;

  d->desc[idx[0]].addr = (uint64)req;
  d->desc[idx[0]].len = sizeof(struct virtio_blk_req);
  d->desc[idx[0]].flags = VRING_DESC_F_NEXT;
  d->desc[idx[0]].next = idx[1];

  // Descriptor 1: data buffer.
  d->desc[idx[1]].addr = (uint64)data;
  d->desc[idx[1]].len = len;

  if (write)
    d->desc[idx[1]].flags = 0;
  else
    d->desc[idx[1]].flags = VRING_DESC_F_WRITE;

  d->desc[idx[1]].flags |= VRING_DESC_F_NEXT;
  d->desc[idx[1]].next = idx[2];

  // Descriptor 2: device status.
  d->info[idx[0]].status = 0xff;
  d->info[idx[0]].done = &done;
  d->info[idx[0]].b = 0;

  d->desc[idx[2]].addr = (uint64)&d->info[idx[0]].status;

  d->desc[idx[2]].len = 1;
  d->desc[idx[2]].flags = VRING_DESC_F_WRITE;
  d->desc[idx[2]].next = 0;

  // Submit request.
  d->avail->ring[d->avail->idx % NUM] = idx[0];

  io_fence();

  d->avail->idx++;

  io_fence();

  *R(d, VIRTIO_MMIO_QUEUE_NOTIFY) = 0;

  // Wait for completion.
  while (!done) {
    proc_vm_io_wait(1);

    sleep_prepare((void *)&done);
    release(&d->vdisk_lock);

    sleep();

    acquire(&d->vdisk_lock);
    proc_vm_io_wait(0);
  }

  if (d->info[idx[0]].status != 0)
    panic("virtio request failed");

  d->info[idx[0]].done = 0;

  free_chain(d, idx[0]);

  release(&d->vdisk_lock);
}

// Existing filesystem interface.
// Always uses device 0.
void
virtio_disk_rw(struct buf *b, int write)
{
  uint64 sector = b->blockno * (BSIZE / SECTOR_SIZE);

  b->disk = 1;

  virtio_rw(&disks[0], b->data, BSIZE, sector, write);

  b->disk = 0;
}

// Swap interface.
// slot identifies a page in swap.img.
// One page = 8 sectors.
void
virtio_swap_rw(void *data, uint slot, int write)
{
  uint64 sector;
  uint64 capacity_sectors;

  // Validate arguments.
  if (data == 0)
    panic("virtio_swap_rw: null buffer");

  if (slot >= SWAP_SLOT_COUNT)
    panic("virtio_swap_rw: invalid slot");

  if (write != 0 && write != 1)
    panic("virtio_swap_rw: invalid operation");

  // Calculate the sector range occupied by this page.
  sector = (uint64)slot * SECTORS_PER_PAGE;
  capacity_sectors = SWAP_DISK_SIZE_BYTES / SECTOR_SIZE;

  if (sector > capacity_sectors || SECTORS_PER_PAGE > capacity_sectors - sector)
    panic("virtio_swap_rw: request out of range");

  virtio_rw(&disks[1], data, PGSIZE, sector, write);
}

// Handle completion interrupt from one device.
// dev = 0 for fs.img, dev = 1 for swap.img.
void
virtio_disk_intr(int dev)
{
  if (dev < 0 || dev >= NDISK)
    panic("virtio interrupt device");

  struct disk *d = &disks[dev];

  acquire(&d->vdisk_lock);

  *R(d, VIRTIO_MMIO_INTERRUPT_ACK) = *R(d, VIRTIO_MMIO_INTERRUPT_STATUS) & 0x3;

  io_fence();

  while (d->used_idx != d->used->idx) {
    io_fence();

    uint32 id = d->used->ring[d->used_idx % NUM].id;

    // Validate the descriptor ID before accessing info[].
    if (id >= NUM)
      panic("virtio interrupt: invalid descriptor id");

    if (d->info[id].status != 0)
      panic("virtio interrupt status");

    volatile int *done = d->info[id].done;

    if (done == 0)
      panic("virtio missing completion");

    *done = 1;

    wakeup((void *)done);

    d->used_idx++;
  }

  release(&d->vdisk_lock);
}
