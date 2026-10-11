char buffer[4] = { 5, 6, 7, 8 };

int run(void)
{
    char __far *where = (char __far *)buffer;
    char __huge *wide = (char __huge *)buffer;
    int first = where[1];
    int second = wide[2];
    return first + second;
}
