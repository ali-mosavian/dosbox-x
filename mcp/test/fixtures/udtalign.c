/* udtalign.c -- the same idea as udtprobe.c, compiled with word alignment so
 * Padded carries a hole its member records do not mention. */

#include <stdio.h>

struct Padded {
    char c;
    long v;
};

struct Padded padded = { 'A', 7L };

int main(void)
{
    printf("c=%c v=%ld size=%d\n", padded.c, padded.v, (int)sizeof(padded));
    return 0;
}
