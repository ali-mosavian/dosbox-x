struct pt {
    int x;
    int y;
};

int counter = 7;

int add(int first, int second)
{
    int sum = first + second;
    counter += sum;
    return sum + counter;
}

int run(void)
{
    struct pt p;
    p.x = 3;
    p.y = add(p.x, 4);
    return p.y;
}
