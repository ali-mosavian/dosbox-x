int sink(long long wide, int limit, int *cell)
{
    return (int)(wide >> 32) + limit + *cell;
}
