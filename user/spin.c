#include "kernel/types.h"
#include "user/user.h"

int main()
{
    int pid = fork();
    char c = '\0';
    if(pid == 0)
    {
        c = '\\';
    }
    else
    {
        printf("parent process id %d , child process id %d\n",getpid(),pid);
        c = '/';
    }

    int cnt = 0;
    while(1)
    {
        if((cnt % 100000)==0)
            write(2,&c,1);
        ++cnt;
    }
    exit(0);
}