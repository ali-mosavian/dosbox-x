int total = 1;

int inner(int n)
{
    int i;
    int sum = 0;
    for (i = 0; i < n; i++)
        sum += i;
    return sum;
}

int outer(void)
{
    int a = inner(100);
    int b = inner(100);
    int c = inner(100);
    return a + b + c;
}

int run(void)
{
    total = outer();
    return total;
}
