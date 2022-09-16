#include "kernel/types.h"
#include "kernel/param.h"
#include "user.h"

#define BUF_LEN (1024)

int readline(int fd, char *buf);
void parse_args(char *buf, int argc, char **argv);

int main(int argc, char **argv)
{
    char *(new_argv[MAXARG]);
    for (int i = 1; i < argc; ++i)
    {
        new_argv[i] = argv[i];
    }

    char argbuf[BUF_LEN];
    while (1)
    {
        memset(argbuf, 0, sizeof(argbuf));
        int is_end = readline(0, argbuf);
        int new_argc = argc;
        parse_args(argbuf, new_argc, new_argv);

        if (fork() == 0)
        {
            exec(new_argv[1], new_argv + 1);
            exit(0);
        }
        else
        {
            if (is_end)
                break;
        }
    }
    while (wait(0) != -1)
    {
    }
    exit(0);
}

int readline(int fd, char *buf)
{
    for (int i = 0;; ++i)
    {
        int num = read(fd, buf + i, 1);
        if (num != 1)
        {
            buf[i] = '\0';
            return 1;
        }
        else if (buf[i] == '\n')
        {
            buf[i] = '\0';
            break;
        }
    }
    return 0;
}

void parse_args(char *buf, int argc, char **argv)
{
    int is_at_space = 1;
    for (int i = 0; buf[i] != '\0'; ++i)
    {
        if (is_at_space && buf[i] != ' ')
        {
            argv[argc++] = buf + i;
            is_at_space = 0;
        }
        if (!is_at_space && buf[i] == ' ')
        {
            buf[i] = '\0';
            is_at_space = 1;
        }
    }
    return;
}