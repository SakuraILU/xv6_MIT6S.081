#include "kernel/types.h"
#include "user.h"
#include "kernel/stat.h"
#include "kernel/fs.h"

void find(char *curname, char *filename)
{
    char buf[512], delim[] = "/";
    int fd;
    struct dirent de;
    struct stat st;

    if ((fd = open(curname, 0)) < 0)
    {
        fprintf(2, "find: cannot open %s\n", curname);
        close(fd);
        return;
    }

    if (fstat(fd, &st) < 0)
    {
        fprintf(2, "find: cannot stat %s\n", curname);
        close(fd);
        return;
    }

    switch (st.type)
    {
    case T_FILE:
        if (strcmp(curname + strlen(curname) - strlen(filename), filename) == 0)
        {
            printf("%s\n", curname);
        }
        close(fd);
        return;
    case T_DIR:
        if (strlen(curname) + 1 + DIRSIZ + 1 > sizeof(buf))
        {
            close(fd);
            return;
        }

        while (read(fd, &de, sizeof(de)))
        {
            if (de.inum == 0)
                continue;
            if (strcmp(de.name + strlen(de.name) - 1, ".") == 0 || strcmp(de.name + strlen(de.name) - 2, "..") == 0)
                continue;

            memset(buf, 0, sizeof(buf));
            strcpy(buf, curname);
            strcat(buf, delim);
            strcat(buf, de.name);
            find(buf, filename);
        }
        break;
    default:
        break;
    }
    close(fd);
    return;
}

int main(int argc, char **argv)
{
    if (argc != 3)
    {
        printf("invalid argument\nUsage: find [top dirname] [filename]\n");
        exit(1);
    }

    find(argv[1], argv[2]);
    exit(0);
}