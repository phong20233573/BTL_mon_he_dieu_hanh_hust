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

  printf("=== SWAP SYSCALL TEST ===\n");

  // TC1: Đọc thống kê Swap thực tế.
  ret = swap_stats(&st);

  if (ret == 0) {
    printf("Policy=%d, Faults=%d, Out=%d, In=%d, Slots=%d\n",
           st.current_policy,
           (int)st.page_faults,
           (int)st.swap_out_count,
           (int)st.swap_in_count,
           (int)st.used_slots);
  }

  check("TC1: swap_stats voi con tro hop le",
        ret == 0 && st.current_policy == POLICY_FIFO);

  // TC2: Chọn FIFO - chính sách hiện đang được hỗ trợ.
  ret = swap_policy(POLICY_FIFO);
  swap_stats(&st);

  check("TC2: Chon chinh sach FIFO",
        ret == 0 && st.current_policy == POLICY_FIFO);

  // TC3: Từ chối CLOCK và các mã chính sách không hợp lệ.
  int ret_clock = swap_policy(POLICY_CLOCK);
  int ret_neg = swap_policy(-1);
  int ret_high = swap_policy(99);

  check("TC3: Tu choi chinh sach chua ho tro va ma khong hop le",
        ret_clock == -1 && ret_neg == -1 && ret_high == -1);

  // TC4: Con trỏ NULL.
  ret = swap_stats((struct swap_stat *)0x0);

  check("TC4: Tu choi con tro NULL",
        ret == -1);

  // TC5: Địa chỉ Kernel và địa chỉ chưa ánh xạ.
  int ret_kern = swap_stats((struct swap_stat *)0x80000000L);
  int ret_unmap = swap_stats((struct swap_stat *)0x3FFFFFFFFFL);

  check("TC5: Tu choi dia chi khong hop le",
        ret_kern == -1 && ret_unmap == -1);

  printf("=== END SWAP SYSCALL TEST ===\n");

  exit(0);
}
