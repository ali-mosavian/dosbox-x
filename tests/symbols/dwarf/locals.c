struct pt {
    int x;
    int y;
};

int counter = 7;
char banner[48] = "symbol probe: the quick brown fox jumps over!";

int add(int first, int second)
{
    int sum = first + second;
    counter += sum;
    return sum + counter;
}

int run(void)
{
    struct pt p;
    int values[3];
    p.x = 3;
    values[0] = 11;
    values[1] = 22;
    values[2] = 33;
    p.y = add(p.x, 4);
    return p.y + values[2];
}
