// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  struct run *freelist;
} kmem;
#define NFRAMES ((PHYSTOP - KERNBASE) / PGSIZE)
struct rmap_entry {
    pagetable_t pagetable;// con trỏ đến bảng trang của tiến trình đang sử dụng frame này
    uint64 va;// địa chỉ ảo tương ứng với frame này trong tiến trình đó
    int pinned;// đánh dấu frame có được "ghim" (pinned) hay không, nghĩa là không thể bị thay thế
    uint64 load_time;// thời điểm frame được nạp vào bộ nhớ, dùng để hỗ trợ thuật toán thay thế trang FIFO
    int valid;// đánh dấu frame có hợp lệ hay không, tức là có thông tin metadata hợp lệ trong rmap_entry
};
static struct rmap_entry rmap[NFRAMES];// mảng rmap lưu trữ thông tin quản lý cho từng frame vật lý trong hệ thống. Mỗi phần tử của mảng tương ứng với một frame và chứa thông tin về bảng trang, địa chỉ ảo, trạng thái pinned, thời điểm nạp và tính hợp lệ của frame đó.

static struct spinlock rmap_lock;
static uint64 rmap_clock = 0;
//rmap[i]: thông tin quản lý frame thứ i.
//rmap_lock: bảo vệ mảng RMAP khi nhiều CPU cùng truy cập.
//rmap_clock: bộ đếm thứ tự đăng ký mapping, phục vụ FIFO.
//valid: xác định frame có đang được cấp phát và có metadata hợp lệ hay không
static int
rmap_index(uint64 pa)
{
    if (pa < KERNBASE || pa >= PHYSTOP)
        return -1;

    if (pa % PGSIZE != 0)
        return -1;

    return (int)((pa - KERNBASE) / PGSIZE);
}// rmap_index: trả về chỉ số của frame trong mảng rmap, dựa vào địa chỉ vật lý pa. Trả về -1 nếu pa không hợp lệ.
void
rmap_mark_allocated(uint64 pa)
{
    int idx = rmap_index(pa);

    if (idx < 0)
        panic("rmap_mark_allocated");

    acquire(&rmap_lock);

    rmap[idx].pagetable = 0;
    rmap[idx].va = 0;
    rmap[idx].pinned = 1;
    rmap[idx].load_time = 0;
    rmap[idx].valid = 1;

    release(&rmap_lock);
}//đánh dấu mặc định pinned = 1 giúp loại những trang này khỏi danh sách ứng viên FIFO. Vì kalloc() không chỉ cấp phát frame cho mỗi user process. 
void
kinit()
{
  initlock(&kmem.lock, "kmem");
  initlock(&rmap_lock, "rmap");
  memset(rmap, 0, sizeof(rmap));
  rmap_clock = 0;
  freerange(end, (void *)PHYSTOP);
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char *)PGROUNDUP((uint64)pa_start);
  for (; p + PGSIZE <= (char *)pa_end; p += PGSIZE)
    kfree(p);
}

// Free the page of physical memory pointed at by pa,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
    struct run *r;

    if (((uint64)pa % PGSIZE) != 0 ||
        (char *)pa < end ||
        (uint64)pa >= PHYSTOP)
        panic("kfree");

    // Remove frame metadata before returning it to free list.
    rmap_unregister((uint64)pa);

    memset(pa, 1, PGSIZE);

    r = (struct run *)pa;

    acquire(&kmem.lock);
    r->next = kmem.freelist;
    kmem.freelist = r;
    release(&kmem.lock);
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
    struct run *r;

    acquire(&kmem.lock);
    r = kmem.freelist;

    if (r)
        kmem.freelist = r->next;

    release(&kmem.lock);

    if (r) {
        rmap_mark_allocated((uint64)r);//khởi tạo 
        memset((char *)r, 5, PGSIZE);
    }

    return (void *)r;
}
int
rmap_register(uint64 pa, pagetable_t pt, uint64 va)
{
    int idx = rmap_index(pa);//lấy chỉ số của frame trong mảng rmap

    if (idx < 0 || pt == 0 || va % PGSIZE != 0)
        return -1;

    acquire(&rmap_lock);//chiếm khóa

    struct rmap_entry *e = &rmap[idx];//lấy con trỏ đến entry trong mảng rmap tương ứng với frame

    // Already mapped: allow an identical registration,
    // but reject a different mapping for the same frame.
    if (e->valid && e->pagetable != 0) {
        if (e->pagetable == pt && e->va == va) {
            release(&rmap_lock);
            return 0;
        }

        release(&rmap_lock);
        return -1;
    }

    e->pagetable = pt;
    e->va = va;
    e->pinned = 0;
    e->load_time = ++rmap_clock;
    e->valid = 1;

    release(&rmap_lock);
    printk("[RMAP+] PA=%p PT=%p VA=%p\n",
       (void *)pa, (void *)pt, (void *)va);
    return 0;
}//rmap_register: đăng ký mapping từ frame vật lý pa sang bảng trang pt và địa chỉ ảo va. Nếu frame đã được mapping, chỉ cho phép đăng ký lại với cùng bảng trang và địa chỉ ảo. Trả về 0 nếu thành công, -1 nếu thất bại.
//ví dụ: rmap_register(0x80002000, PT_A, 0x1000); thì rmap[2]{ pagetable = PT_A,
    //va        = 0x1000,
    //pinned    = 0,
    //load_time = 1,
    //valid     = 1}
void
rmap_unregister(uint64 pa)
{
    int idx = rmap_index(pa);

    if (idx < 0)
        return;

    acquire(&rmap_lock);

    rmap[idx].pagetable = 0;
    rmap[idx].va = 0;
    rmap[idx].pinned = 0;
    rmap[idx].load_time = 0;
    rmap[idx].valid = 0;

    release(&rmap_lock);
}//rmap_unregister: hủy đăng ký mapping của frame vật lý pa. Thiết lập các trường trong entry tương ứng trong mảng rmap về giá trị mặc định.
void
rmap_pin(uint64 pa)
{
    int idx = rmap_index(pa);

    if (idx < 0)
        return;

    acquire(&rmap_lock);

    if (rmap[idx].valid) {
        rmap[idx].pagetable = 0;
        rmap[idx].va = 0;
        rmap[idx].pinned = 1;
        rmap[idx].load_time = 0;
    }

    release(&rmap_lock);
}//rmap_pin(): frame vẫn đang được cấp phát nhưng không còn là User Page có thể swap.
uint64
select_victim_frame(void)
{
    uint64 victim = 0;
    uint64 oldest = (uint64)-1;

    acquire(&rmap_lock);

    for (int i = 0; i < NFRAMES; i++) {
        if (rmap[i].valid &&
            rmap[i].pinned == 0 &&
            rmap[i].pagetable != 0) {

            if (rmap[i].load_time < oldest) {
                oldest = rmap[i].load_time;
                victim = KERNBASE + (uint64)i * PGSIZE;
            }
        }
    }

    release(&rmap_lock);

    return victim;
}//select_victim_frame: chọn một frame vật lý để thay thế theo thuật toán FIFO. Duyệt qua mảng rmap, tìm frame hợp lệ, không bị pinned và có bảng trang. Chọn frame có load_time nhỏ nhất (cũ nhất) làm nạn nhân. Trả về địa chỉ vật lý của frame nạn nhân, hoặc 0 nếu không tìm thấy.
int
rmap_count_pt(pagetable_t pt)
{
    int count = 0;

    acquire(&rmap_lock);

    for (int i = 0; i < NFRAMES; i++) {
        if (rmap[i].valid &&
            rmap[i].pagetable == pt) {
            count++;
        }
    }

    release(&rmap_lock);

    return count;
}//rmap_count_pt: đếm số lượng frame vật lý đang được mapping bởi bảng trang pt. Duyệt qua mảng rmap, nếu entry hợp lệ và có bảng trang bằng pt, tăng biến đếm. Trả về số lượng frame được mapping bởi pt.
