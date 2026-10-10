int counter = 1;
char banner[48] = "symbol probe: the quick brown fox jumps over!";

int twice(int v)
{
    return v * 2;
}

int bump(int n)
{
    int delta = twice(n);
    counter += delta;
    return counter;
}

int run(void)
{
    bump(5);
    return bump(7);
}
