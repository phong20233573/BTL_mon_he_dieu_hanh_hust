#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/swap.h"
#include "user/user.h"

#define PGSIZE 4096
#define WORDS_PER_PAGE (PGSIZE / sizeof(uint))

static int fail_count = 0;

void
check(char *test_name, int condition)
{
  if (condition) {
    printf("[PASS] %s\n", test_name);
  } else {
    printf("[FAIL] %s\n", test_name);
    fail_count++;
  }
}
//--------------------------------------------Khởi tạo trước các hàm phụ trợ cho các kịch bản test----------------------------------------------------------------------------------
static uint
make_pattern(int page, int word)
{
  return 0xA5A50000U ^
         ((uint)page * 2654435761U) ^
         ((uint)word * 2246822519U);
} // Tạo mẫu dữ liệu giả lập dựa trên số trang và số từ trong trang. Mẫu này được sử dụng để kiểm tra tính toàn vẹn của dữ liệu khi đọc/ghi vào bộ nhớ ảo.
static void
write_page(char *base, int page)
{
  volatile uint *ptr =
      (volatile uint *)(base + page * PGSIZE);

  for (int j = 0; j < WORDS_PER_PAGE; j++) {
    ptr[j] = make_pattern(page, j);
  }
}// Ghi dữ liệu giả lập vào một trang cụ thể trong vùng nhớ ảo. Mỗi từ trong trang được ghi bằng mẫu dữ liệu được tạo ra từ hàm make_pattern.
//page là số trang (0..N-1) và base là địa chỉ cơ sở của vùng nhớ ảo.
/* 
-1 frame vật lý(hay là page ở hàm trên) = 4096 bytes = 1024 words = 1024 * 4 bytes
-nếu ta gọi write_page(base,2) thì hàm sẽ ghi dữ liệu giả lập vào frame số 2(ghi đầy)
-volatitle nghĩa là dữ liệu có thể thay đổi bất cứ lúc nào, nên trình biên dịch sẽ không tối ưu hóa việc truy cập vào con trỏ ptr, đảm bảo rằng mỗi lần truy cập sẽ thực sự đọc/ghi vào bộ nhớ.
*/
static int
verify_page(char *base, int page)
{
  volatile uint *ptr =
      (volatile uint *)(base + page * PGSIZE);

  for (int j = 0; j < WORDS_PER_PAGE; j++) {
    uint expected = make_pattern(page, j);
    uint actual = ptr[j];

    if (actual != expected) {
      printf("[DATA ERROR] Page=%d Word=%d\n", page, j);
      printf("Expected=%x Actual=%x\n", expected, actual);
      return 0;
    }
  }

  return 1;
}// Kiểm tra tính toàn vẹn của dữ liệu trong một trang cụ thể trong vùng nhớ ảo. So sánh từng từ trong trang với mẫu dữ liệu dự kiến được tạo ra từ hàm make_pattern. Nếu tất cả các từ khớp, trả về 1 (thành công), nếu có sự khác biệt, in ra thông báo lỗi và trả về 0 (thất bại).

static int
get_swap_stats(struct swap_stat *st)
{
  if (swap_stats(st) < 0) {
    printf("[ERROR] swap_stats failed\n");
    return 0;
  }

  return 1;
}
// static void
// print_swap_stats(struct swap_stat *st)
// {
//   printf("Page faults : %d\n", (int)st->page_faults);
//   printf("Swap out    : %d\n", (int)st->swap_out_count);
//   printf("Swap in     : %d\n", (int)st->swap_in_count);
//   printf("Used slots  : %d\n", (int)st->used_slots);
// }
//------------------- Các kịch bản test ------------------------------------------------------------------------------------------------
static void
test_basic_memory(int npages)
{
    printf("\n=== TEST 1: BASIC MEMORY ===\n");

    // 1. Calculate the requested memory size.
    int bytes = npages * PGSIZE;

    printf("[TEST 1] Requesting %d pages (%d bytes)\n",
           npages, bytes);

    // 2. Allocate memory using sbrk().
    char *base = sbrk(bytes);

    if (base == (char *)-1) {
        check("TC1.1: Memory allocation", 0);
        return;
    }

    printf("[TEST 1] Allocated at VA=%p\n", base);

    check("TC1.1: Memory allocation", 1);

    // 3. Write different data to every page.
    printf("[TEST 1] Writing data...\n");

    for (int i = 0; i < npages; i++) {

        volatile uint *ptr =
            (volatile uint *)(base + i * PGSIZE);

        for (int j = 0; j < WORDS_PER_PAGE; j++) {
            ptr[j] = make_pattern(i, j);
        }
    }

    check("TC1.2: Write data to all pages", 1);

    // 4. Read and verify all pages.
    printf("[TEST 1] Verifying data...\n");

    int valid = 1;

    for (int i = 0; i < npages; i++) {

        volatile uint *ptr =
            (volatile uint *)(base + i * PGSIZE);

        for (int j = 0; j < WORDS_PER_PAGE; j++) {

            uint expected = make_pattern(i, j);
            uint actual = ptr[j];

            if (actual != expected) {

                printf("[DATA ERROR] Page=%d Word=%d\n",
                       i, j);

                printf("Expected=%x Actual=%x\n",
                       expected, actual);

                valid = 0;
                break;
            }
        }

        if (!valid)
            break;
    }

    check("TC1.3: Read and verify all pages", valid);

    printf("=== END TEST 1 ===\n");
}

static int
allocate_and_fill(int npages, char **base_out)
{
  char *cur = sbrk(0);

  if (cur == (char *)-1)
    return -1;

  // Align heap break to a page boundary.
  uint64 addr = (uint64)cur;
  uint64 aligned =
      (addr + PGSIZE - 1) & ~((uint64)PGSIZE - 1);

  if (aligned != addr) {
    int padding = (int)(aligned - addr);

    if (sbrk(padding) == (char *)-1)
      return -1;
  }

  char *base = sbrk(0);

  if (base == (char *)-1)
    return -1;

  *base_out = base;

  for (int i = 0; i < npages; i++) {

    char *page_addr = sbrk(PGSIZE);

    if (page_addr == (char *)-1) {
      printf("[ALLOC] Failed at page %d\n", i);
      return i;
    }

    // Check that allocation is page-aligned
    // and follows the expected address sequence.
    if ((uint64)page_addr !=
        (uint64)base + (uint64)i * PGSIZE) {
      printf("[ALLOC] Unexpected address at page %d\n", i);
      return i;
    }

    write_page(base, i);
  }

  return npages;
}
static void
test_swap_out(int npages)
{
  struct swap_stat before, after;
  char *base = 0;

  printf("\n=== TEST 2: SWAP OUT ===\n");

  if (!get_swap_stats(&before)) {
    check("TC2: Read stats before", 0);
    return;
  }

  int allocated = allocate_and_fill(npages, &base);

  if (!get_swap_stats(&after)) {
    check("TC2: Read stats after", 0);
    return;
  }

  check("TC2.1: Allocate requested pages",
        allocated == npages);

  check("TC2.2: Swap-out occurred",
        after.swap_out_count > before.swap_out_count);

  printf("[BEFORE] Swap out=%d, Used slots=%d\n",
         (int)before.swap_out_count,
         (int)before.used_slots);

  printf("[AFTER] Swap out=%d, Used slots=%d\n",
         (int)after.swap_out_count,
         (int)after.used_slots);
}

static void
test_swap_in(int npages)
{
  struct swap_stat before, after_out, after_in;
  char *base = 0;

  printf("\n=== TEST 3: SWAP IN ===\n");

  if (!get_swap_stats(&before))
    return;

  int allocated = allocate_and_fill(npages, &base);

  if (!get_swap_stats(&after_out))
    return;

  check("TC3.1: Pressure caused swap-out",
        allocated == npages &&
        after_out.swap_out_count > before.swap_out_count);

  if (allocated != npages)
    return;

  /*
   * Read the earlier allocated pages first.
   * Under FIFO they are among the older candidate pages.
   */
  int valid = 1;
  int test_pages = npages < 16 ? npages : 16;

  for (int i = 0; i < test_pages; i++) {
    if (!verify_page(base, i)) {
      valid = 0;
      break;
    }
  }

  check("TC3.2: Read previous page data", valid);

  if (!get_swap_stats(&after_in))
    return;

  check("TC3.3: Swap-in occurred",
        after_in.swap_in_count > after_out.swap_in_count);

  printf("[SWAP-IN] Before=%d After=%d\n",
         (int)after_out.swap_in_count,
         (int)after_in.swap_in_count);
}
void test_api(){
struct swap_stat st;
  int ret;

  printf("=== BAT DAU KIEM THU TUAN 1: SYSCALL INTERFACE ===\n");

  // TC1: Đọc dữ liệu giả lập ban đầu bằng con trỏ hợp lệ (&st)
  ret = swap_stats(&st);
  printf(
    " -> Du lieu doc tu Kernel: policy=%d, faults=%d, out=%d, in=%d, slots=%d\n",
    st.current_policy, (int)st.page_faults, (int)st.swap_out_count,
    (int)st.swap_in_count, (int)st.used_slots);
  check("TC1: swap_stats(&st) voi con tro hop le",
        ret == 0 && st.page_faults == 10);

  // TC2: Chuyển thuật toán hợp lệ sang CLOCK (1) và kiểm tra bộ đếm đã reset về 0
  ret = swap_policy(POLICY_CLOCK);
  swap_stats(&st);
  check("TC2: swap_policy(POLICY_CLOCK) cap nhat & reset bo dem",
        ret == 0 && st.current_policy == POLICY_CLOCK && st.page_faults == 0);

  // TC3: Truyền mã thuật toán sai (ngoài khoảng 0..3)
  int ret_neg = swap_policy(-1);
  int ret_high = swap_policy(99);
  check("TC3: Tu choi ma thuat toan khong hop le (-1 va 99)",
        ret_neg == -1 && ret_high == -1);

  // TC4: Truyền con trỏ NULL (0x0) vào swap_stats
  ret = swap_stats((struct swap_stat *)0x0);
  check("TC4: Bat loi con tro NULL (0x0) an toan", ret == -1);

  // TC5: Truyền địa chỉ vùng nhớ Kernel (0x80000000) và địa chỉ chưa cấp phát
  int ret_kern = swap_stats((struct swap_stat *)0x80000000L);
  int ret_unmap = swap_stats((struct swap_stat *)0x3FFFFFFFFFL);
  check("TC5: Chan truy cap dia chi Kernel & dia chi chua anh xa",
        ret_kern == -1 && ret_unmap == -1);
}
static int
verify_all_pages_reverse(char *base, int npages)
{
  for (int i = npages - 1; i >= 0; i--) {

    if (!verify_page(base, i)) {
      printf("[DATA ERROR] Page %d corrupted\n", i);
      return 0;
    }
  }

  return 1;
}
static void
test_data_integrity(int npages)
{
  struct swap_stat before, after_out, after_in;
  char *base = 0;

  printf("\n=== TEST 4: DATA INTEGRITY ===\n");

  if (!get_swap_stats(&before))
    return;

  int allocated = allocate_and_fill(npages, &base);

  if (!get_swap_stats(&after_out))
    return;

  check("TC4.1: All pages allocated",
        allocated == npages);

  check("TC4.2: Swap-out occurred",
        after_out.swap_out_count > before.swap_out_count);

  if (allocated != npages)
    return;

  int valid = verify_all_pages_reverse(base, npages);

  check("TC4.3: All page contents preserved", valid);

  if (!get_swap_stats(&after_in))
    return;

  check("TC4.4: Swap-in occurred",
        after_in.swap_in_count > after_out.swap_in_count);
}










//------------------------------------------------------------------------------------------------------------------------------
int
main(int argc, char *argv[])
{
 
    if (argc < 2) {
    printf("Usage:\n");
    printf("my_test api\n");
    printf("my_test basic <pages>\n");
    printf("my_test out <pages>\n");
    printf("my_test in <pages>\n");
    printf("my_test integrity <pages>\n");
    printf("my_test reuse <pages>\n");
    exit(1);
  }

  int npages = 8;

  if (argc >= 3)
    npages = atoi(argv[2]);

  if (!strcmp(argv[1], "api")) {
    // Chạy 5 test System Call hiện tại.
    // Chuyển code kiểm thử cũ vào hàm test_api().
    test_api();
  }
  else if (npages <= 0) {
    printf("[FAIL] Invalid page count\n");
    exit(1);
  }
  else if (!strcmp(argv[1], "basic")) {
    test_basic_memory(npages);
  }
  else if (!strcmp(argv[1], "out")) {
    test_swap_out(npages);
  }
  else if (!strcmp(argv[1], "in")) {
    test_swap_in(npages);
  }
  else if (!strcmp(argv[1], "integrity")) {
    test_data_integrity(npages);
  }
  // else if (!strcmp(argv[1], "reuse")) {
  //   test_slot_reuse(npages);
  // }
  else {
    printf("[FAIL] Unknown test\n");
    exit(1);
  }

  if (fail_count == 0) {
    printf("\nALL MY_TESTS PASSED\n");
    exit(0);
  }

  printf("\nMY_TEST FAILED: %d errors\n", fail_count);
  exit(1);
}