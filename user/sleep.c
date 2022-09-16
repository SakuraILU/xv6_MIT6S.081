#include "kernel/types.h"
#include "user.h"

int is_num(char *str)
{
    for (int i = 0; i < strlen(str); ++i)
    {
        if (!(str[i] >= '0' && str[i] <= '9'))
            return 0;
    }
    return 1;
}

int main(int argc, char **argv)
{
    if (argc == 0)
        printf("sleep: missing operand\nTry 'sleep --help' for more information.\n");
    else if (!is_num(argv[1]))
        printf("sleep: invalid time interval \'%s\'\nTry 'sleep --help' for more information.\n", argv[1]);
    else
        sleep(atoi(argv[1]));

    exit(0);
}