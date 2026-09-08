/* tdsprobe.c -- fixture source for a Borland Turbo Debugger symbol parser.
 *
 * Deliberately plain: two leaf functions plus main, one global scalar, one
 * global array, locals in every function, one statement per line so the
 * line-number table has something to say.
 */

#include <stdio.h>

int g_counter = 7;
int g_table[8] = { 1, 2, 3, 5, 8, 13, 21, 34 };

int tp_sum(int n)
{
    int i;
    int total;

    total = 0;
    for (i = 0; i < n; i++) {
        total += g_table[i];
    }
    return total;
}

int tp_scale(int v, int k)
{
    int r;

    r = v * k;
    r = r + g_counter;
    return r;
}

int main(void)
{
    int s;
    int t;

    s = tp_sum(8);
    t = tp_scale(s, 3);
    printf("sum=%d scaled=%d\n", s, t);
    return 0;
}
