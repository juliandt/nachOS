#include "syscall.h"

#define NULL        ((void*)0)

int main(){
    int fid;
   char *argv[] = {"holaa", "mundo", "!!!!!", "asddd", "qweee", NULL};   
   fid = Exec("../test/print_args", 5, argv, 0);
   Exit(0);
}
