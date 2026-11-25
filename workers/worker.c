#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int parse_int(const char *s, int *out) {
    char *end = NULL;
    long val;

    if (s == NULL || s[0] == '\0') {
        return -1;
    }

    errno = 0;
    val = strtol(s, &end, 10);
    if (errno != 0 || *end != '\0' || val < INT_MIN || val > INT_MAX) {
        return -1;
    }

    *out = (int)val;
    return 0;
}

static void print_usage(const char *prog) {
    fprintf(stderr,
            "Usage: %s <name> <lifetime_sec> <exit_code> <ignore_sigterm>\n",
            prog);
}

int main(int argc, char *argv[]) {
    int lifetime_sec = 0;
    int exit_code = 0;
    int ignore_sigterm = 0;
    struct timespec start;

    if (argc != 5 || argv[1][0] == '\0' ||
        parse_int(argv[2], &lifetime_sec) != 0 ||
        parse_int(argv[3], &exit_code) != 0 ||
        parse_int(argv[4], &ignore_sigterm) != 0 ||
        lifetime_sec < 0 || exit_code < 0 || exit_code > 255 ||
        (ignore_sigterm != 0 && ignore_sigterm != 1)) {
        print_usage(argc > 0 ? argv[0] : "worker");
        return 64;
    }

    if (ignore_sigterm == 1) {
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = SIG_IGN;
        if (sigemptyset(&sa.sa_mask) == -1) {
            perror("sigemptyset");
            return 1;
        }
        sa.sa_flags = 0;
        if (sigaction(SIGTERM, &sa, NULL) == -1) {
            perror("sigaction");
            return 1;
        }
    }

    if (clock_gettime(CLOCK_MONOTONIC, &start) == -1) {
        perror("clock_gettime");
        return 1;
    }

    for (;;) {
        if (lifetime_sec > 0) {
            struct timespec now;
            long elapsed;

            if (clock_gettime(CLOCK_MONOTONIC, &now) == -1) {
                perror("clock_gettime");
                return 1;
            }
            elapsed = (long)(now.tv_sec - start.tv_sec);
            if (now.tv_nsec < start.tv_nsec) {
                elapsed--;
            }
            if (elapsed >= lifetime_sec) {
                return exit_code;
            }
        }
        (void)sleep(1);
    }
}
