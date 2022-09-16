#include "kernel/types.h"
#include "user.h"

void print_primes(int pip_in);

int main(int argc, char **argv)
{
    int pip_fd[2];
    pipe(pip_fd);

    if (fork() != 0)
    {
        close(pip_fd[0]);
        for (int i = 2; i < 36; ++i)
        {
            write(pip_fd[1], &i, sizeof(int));
        }
        close(pip_fd[1]);
        wait(0);
    }
    else
    {
        close(pip_fd[1]);
        print_primes(pip_fd[0]);
        close(pip_fd[0]);
    }
    exit(0);
}

void print_primes(int pip_in)
{
    int first_num = 0;
    if (read(pip_in, &first_num, sizeof(first_num)) == 0)
    {
        close(pip_in);
        exit(0);
    }

    int pip_fd[2];
    pipe(pip_fd);
    printf("prime %d\n", first_num);
    if (fork() != 0)
    {
        close(pip_fd[0]);
        int num = 0;
        while (read(pip_in, &num, sizeof(num)) != 0)
            if (num % first_num != 0)
                write(pip_fd[1], &num, sizeof(num));
        close(pip_in);
        close(pip_fd[1]);
        wait(0);
    }
    else
    {
        close(pip_in);
        close(pip_fd[1]);
        print_primes(pip_fd[0]);
        close(pip_fd[0]);
    }
    exit(0);
}