#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

#define TEST_MB 140
#define TEST_SIZE (TEST_MB * 1024 * 1024)
#define PAGE_SIZE 4096

int
main(void)
{
    char *memory;
    int pages;
    int i;
    int errors = 0;

    printf("=== SWAP TEST START ===\n");
    printf("Allocating %d MB of user memory...\n", TEST_MB);

    // Cap phat 140 MB bo nho user
    memory = sbrk(TEST_SIZE);

    if (memory == (char *)-1) {
        printf("ERROR: sbrk failed\n");
        exit(1);
    }

    pages = TEST_SIZE / PAGE_SIZE;

    printf("Allocated %d pages\n", pages);
    printf("Writing data to all pages...\n");

    // Ghi du lieu vao tung trang
    for (i = 0; i < pages; i++) {
        memory[i * PAGE_SIZE] = (char)(i % 251);
    }

    printf("Write completed.\n");
    printf("Reading and checking data...\n");

    // Doc lai va kiem tra du lieu
    for (i = 0; i < pages; i++) {
        char expected = (char)(i % 251);
        char actual = memory[i * PAGE_SIZE];

        if (actual != expected) {
            errors++;

            if (errors <= 10) {
                printf("ERROR at page %d: expected %d, got %d\n",
                       i, expected, actual);
            }
        }
    }

    printf("Checked %d pages\n", pages);
    printf("Data errors: %d\n", errors);

    if (errors == 0) {
        printf("=== SWAP TEST PASSED ===\n");
    } else {
        printf("=== SWAP TEST FAILED ===\n");
    }

    exit(errors == 0 ? 0 : 1);
}
