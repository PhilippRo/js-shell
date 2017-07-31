
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char** argv) {
    if (argc == 3) {
        //great
    } else {
        printf("usage %s [howmany: int] [string]\n", argv[0]);
    }

    for(int i = 0; i < atoi(argv[1]); i++) {
        printf("%s ", argv[2]);
    }
    return 0;
}
