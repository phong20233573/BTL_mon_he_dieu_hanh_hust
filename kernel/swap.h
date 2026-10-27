#ifndef SWAP_H
#define SWAP_H

// Các hằng số mã hóa thuật toán thay thế trang (Page Replacement Policies)
#define POLICY_FIFO       0
#define POLICY_CLOCK      1
#define POLICY_AGING_LRU  2
#define POLICY_WORKSET    3

// Cấu trúc lưu trữ các biến định lượng thống kê hoạt động của hệ thống Swap.
// Dùng chung để Kernel đóng gói dữ liệu và System Call copyout trả về cho User.
struct swap_stat {
  int current_policy;      // Thuật toán đang áp dụng hiện tại (0: FIFO, 1: CLOCK, ...)
  uint64 page_faults;      // Tổng số lần lỗi trang (Truy cập vào trang đã đẩy ra đĩa)
  uint64 swap_out_count;   // Tổng số lần đẩy trang từ RAM xuống đĩa Swap (Eviction)
  uint64 swap_in_count;    // Tổng số lần nạp trang từ đĩa Swap lên lại RAM
  uint64 used_slots;       // Số ô nhớ (4KB/ô) đang bị chiếm dụng trên đĩa virtio thứ 2
};

#endif // SWAP_H