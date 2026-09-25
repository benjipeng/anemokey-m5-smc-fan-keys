#include "hold.h"
#include "probe.h"

#include <stdio.h>
#include <string.h>

static void usage(void) {
    fprintf(stderr, "usage: anemokey\n");
    fprintf(stderr, "       anemokey --rpm <rpm>\n");
}

int main(int argc, char **argv) {
    if (argc == 1) {
        return anemokey_probe();
    }
    if (argc == 2 && (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)) {
        usage();
        return 0;
    }
    if (argc == 3 && strcmp(argv[1], "--rpm") == 0) {
        return anemokey_hold(argv[2]);
    }
    usage();
    return 1;
}
