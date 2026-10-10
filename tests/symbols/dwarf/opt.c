extern int sink(long long wide, int limit, int *cell);

int scale(int n)
{
    int limit = 100;
    long long wide = (long long)n * 3000000000LL;
    int cell = n + 1;
    int total = sink(wide, limit, &cell);
    return total + cell + limit;
}

int run(void)
{
    return scale(5);
}
