#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/swap.h"
#include "user/user.h"

#define TEST_MB   140
#define TEST_SIZE (TEST_MB * 1024 * 1024)
#define PAGE_SIZE 4096

int
main(void)
{
  char *memory;
  int pages;
  int errors = 0;
  uint64 slots_before_unmap;
  struct swap_stat initial, stats;

  printf("=== SWAP TEST START ===\n");
  if (swap_stats(&initial) < 0) {
    printf("ERROR: could not read initial swap statistics\n");
    exit(1);
  }

  printf("Allocating %d MB of user memory...\n", TEST_MB);
  memory = sbrk(TEST_SIZE);
  if (memory == (char *)-1) {
    printf("ERROR: sbrk failed\n");
    exit(1);
  }

  pages = TEST_SIZE / PAGE_SIZE;
  printf("Allocated %d pages\n", pages);
  printf("Writing data to all pages...\n");

  for (int i = 0; i < pages; i++)
    memory[i * PAGE_SIZE] = (char)(i % 251);

  printf("Write completed.\n");
  if (swap_stats(&stats) < 0 || stats.used_slots <= initial.used_slots ||
      stats.swap_out_count <= initial.swap_out_count) {
    printf("ERROR: swap-out did not allocate bitmap slots\n");
    exit(1);
  }
  printf(
    "After swap-out: %d slots currently used; %d swap-outs since test start\n",
    (int)stats.used_slots,
    (int)(stats.swap_out_count - initial.swap_out_count));

  printf("Reading and checking data...\n");
  for (int i = 0; i < pages; i++) {
    char expected = (char)(i % 251);
    char actual = memory[i * PAGE_SIZE];

    if (actual != expected) {
      errors++;
      if (errors <= 10)
        printf("ERROR at page %d: expected %d, got %d\n", i, expected, actual);
    }
  }

  printf("Checked %d pages\n", pages);
  printf("Data errors: %d\n", errors);
  if (swap_stats(&stats) < 0 || stats.swap_in_count <= initial.swap_in_count) {
    printf("ERROR: swap-in did not complete successfully\n");
    exit(1);
  }
  slots_before_unmap = stats.used_slots;
  printf(
    "After swap-in: %d slots currently used; %d swap-ins since test start\n",
    (int)slots_before_unmap,
    (int)(stats.swap_in_count - initial.swap_in_count));

  if (sbrk(-TEST_SIZE) == (char *)-1) {
    printf("ERROR: failed to release test memory\n");
    exit(1);
  }
  if (swap_stats(&stats) < 0 || stats.used_slots >= slots_before_unmap) {
    printf(
      "ERROR: unmapping did not release swap slots (%d remain; %d before)\n",
      (int)stats.used_slots, (int)slots_before_unmap);
    exit(1);
  }
  printf("After unmap: %d fewer slots used; %d remain allocated\n",
         (int)(slots_before_unmap - stats.used_slots), (int)stats.used_slots);
  if (swap_dump() < 0) {
    printf("ERROR: could not dump remaining swap slot owners\n");
    exit(1);
  }

  if (errors == 0)
    printf("=== SWAP TEST PASSED ===\n");
  else
    printf("=== SWAP TEST FAILED ===\n");

  exit(errors == 0 ? 0 : 1);
}
