/* udtprobe.c -- a struct and an array of structs, for reading back by name. */

#include <stdio.h>

struct Point {
    int x;
    int y;
    long tag;
};

struct Point origin = { 3, 4, 100L };
struct Point path[4] = { {1, 2, 10L}, {3, 4, 20L}, {5, 6, 30L}, {7, 8, 40L} };

int sum_x(struct Point *p, int n)
{
    int i;
    int total;

    total = 0;
    for (i = 0; i < n; i++) {
        total += p[i].x;
    }
    return total;
}

int main(void)
{
    struct Point local;
    int s;

    local.x = 11;
    local.y = 12;
    local.tag = 99L;
    s = sum_x(path, 4);
    printf("origin=%d,%d sum=%d local=%ld\n", origin.x, origin.y, s, local.tag);
    return 0;
}
