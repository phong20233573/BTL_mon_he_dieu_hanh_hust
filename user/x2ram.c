#include "kernel/types.h"
#include "kernel/swap.h"
#include "user/user.h"

#define PGSIZE 4096
#define TEST_SIZE (40 * 1024 * 1024)
#define WORDS_PER_PAGE (PGSIZE / sizeof(uint))

static uint
pattern(uint page, uint word)
{
  return 0x5a2d0000U ^ (page * 2654435761U) ^ (word * 2246822519U);
}

int
main(void)
{
  struct swap_stat before, after;
  volatile uint *memory;

  printf("x2ram: requesting %d MiB of user RAM\n", TEST_SIZE / (1024 * 1024));
  if (swap_stats(&before) < 0) {
    printf("x2ram: cannot read swap statistics\n");
    exit(1);
  }

  memory = (volatile uint *)sbrk(TEST_SIZE);
  if ((char *)memory == (char *)-1) {
    printf("x2ram: allocation of 40 MiB failed\n");
    exit(1);
  }

  for (uint page = 0; page < TEST_SIZE / PGSIZE; page++) {
    for (uint word = 0; word < WORDS_PER_PAGE; word++)
      memory[page * WORDS_PER_PAGE + word] = pattern(page, word);
  }

  for (uint page = 0; page < TEST_SIZE / PGSIZE; page++) {
    for (uint word = 0; word < WORDS_PER_PAGE; word++) {
      uint actual = memory[page * WORDS_PER_PAGE + word];
      if (actual != pattern(page, word)) {
        printf("x2ram: data mismatch at page %d, word %d\n", (int)page,
               (int)word);
        exit(1);
      }
    }
  }

  if (swap_stats(&after) < 0) {
    printf("x2ram: cannot read final swap statistics\n");
    exit(1);
  }
  if (after.swap_out_count != before.swap_out_count) {
    printf("x2ram: swap-out occurred during the RAM test\n");
    exit(1);
  }

  printf("x2ram: PASS - 40 MiB verified with no swap-out\n");
  exit(0);
}
