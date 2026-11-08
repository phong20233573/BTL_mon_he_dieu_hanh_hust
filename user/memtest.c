#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

#define PGSIZE 4096
#define TOTAL_PAGES 8192

int
main(int argc, char *argv[])
{
    char *start;
    char *p;
    int i;
    int allocated = 0;

    printf("\n=== XV6 MEMORY TEST ===\n");

    start = sbrk(0);
    if(start == (char *)-1) {
        printf("Cannot get heap address\n");
        exit(1);
    }

    for(i = 0; i < TOTAL_PAGES; i++) {
        p = sbrk(PGSIZE);

        if(p == (char *)-1) {
            printf("Allocation failed at page %d\n", i);
            break;
        }

        p[0] = 'A';
        p[PGSIZE - 1] = 'Z';

        allocated++;

        if(allocated % 64 == 0) {
            printf("Allocated: %d pages (%d KB)\n",
                   allocated, allocated * 4);
        }
    }

    printf("\n=== RESULT ===\n");
    printf("Allocated pages: %d\n", allocated);
    printf("Allocated memory: %d KB\n", allocated * 4);

    for(i = 0; i < allocated; i++) {
        p = start + i * PGSIZE;

        if(p[0] != 'A' || p[PGSIZE - 1] != 'Z') {
            printf("Data verification failed at page %d\n", i);
            exit(1);
        }
    }

    printf("Data verification: PASSED\n");
    exit(0);
}
