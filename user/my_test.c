#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/swap.h"
#include "user/user.h"

void
check(char *test_name, int condition)
{
  if (condition)
    printf("[PASS] %s\n", test_name);
  else
    printf("[FAIL] %s\n", test_name);
}

int
main(int argc, char *argv[])
{
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

  printf("=== KET THUC KIEM THU TUAN 1 ===\n");
  exit(0);
}