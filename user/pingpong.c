#include "kernel/types.h"
#include "user.h"
int main(int argc, char **argv)
{
    int pip_p2c[2], pip_c2p[2];
    pipe(pip_p2c);
    pipe(pip_c2p);

    if (fork() != 0)
    {
        // parent process
        close(pip_p2c[0]);
        close(pip_c2p[1]);

        char buf = 'x';
        write(pip_p2c[1], &buf, 1);
        read(pip_c2p[0], &buf, 1);
        printf("%d: received pong\n", getpid());

        close(pip_p2c[1]);
        close(pip_c2p[0]);
        wait(0);
    }
    else
    {
        // child process
        close(pip_p2c[1]);
        close(pip_c2p[0]);

        char buf;
        read(pip_p2c[0], &buf, 1);
        printf("%d: received ping\n", getpid());
        write(pip_c2p[1], &buf, 1);

        close(pip_p2c[0]);
        close(pip_c2p[1]);
    }
    close(pip_c2p[0]);
    close(pip_c2p[1]);
    close(pip_p2c[0]);
    close(pip_p2c[1]);
    exit(0);
}
