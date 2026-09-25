#include "hold.h"
#include "smc.h"

#include <mach/mach_error.h>

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum { kMaxFans = 9 };

typedef struct {
    int engaged;
    char mode_key[5];
    uint8_t original_mode;
} HeldFan;

static io_connect_t g_conn;
static HeldFan g_fans[kMaxFans];
static unsigned g_fan_count;
static int g_restore_done;
static volatile sig_atomic_t g_stop;

static void fan_key_name(char out[5], int fan, const char *suffix) {
    out[0] = 'F';
    out[1] = (char)('0' + fan);
    out[2] = suffix[0];
    out[3] = suffix[1];
    out[4] = '\0';
}

static int ok_value(const SmcValue *value) {
    return value->kr == KERN_SUCCESS && value->smc_result == 0 && !value->size_rejected;
}

static void on_signal(int sig) {
    (void)sig;
    g_stop = 1;
}

static int restore_fans(void) {
    if (g_restore_done || g_conn == 0) {
        return 0;
    }
    g_restore_done = 1;
    int failed = 0;
    for (unsigned fan = 0; fan < g_fan_count; fan++) {
        HeldFan *held = &g_fans[fan];
        if (!held->engaged) {
            continue;
        }
        uint8_t byte = held->original_mode;
        printf("restore %s %u\n", held->mode_key, byte);
        fflush(stdout);
        for (int attempt = 0; attempt < 5; attempt++) {
            SmcValue wrote = smc_write(g_conn, held->mode_key, &byte, 1);
            if (wrote.kr != KERN_SUCCESS || wrote.smc_result != 0) {
                fprintf(stderr, "restore %s attempt %d: %s (0x%x) smc %u\n",
                        held->mode_key, attempt, mach_error_string(wrote.kr), wrote.kr, wrote.smc_result);
            }
            usleep(200000);
            SmcValue now = smc_read(g_conn, held->mode_key);
            if (ok_value(&now) && now.size >= 1 && now.bytes[0] != 1) {
                printf("restore %s ok mode=%u\n", held->mode_key, now.bytes[0]);
                fflush(stdout);
                held->engaged = 0;
                break;
            }
        }
        if (held->engaged) {
            fprintf(stderr, "restore failed: %s still manual\n", held->mode_key);
            failed = 1;
        }
    }
    return failed;
}

static void restore_on_exit(void) {
    restore_fans();
    smc_close(g_conn);
    g_conn = 0;
}

static int find_mode_key(const uint32_t *keys, size_t key_count, int fan, char out[5]) {
    const char *suffixes[] = {"md", "Md", "MD", "mD"};
    for (size_t i = 0; i < sizeof suffixes / sizeof suffixes[0]; i++) {
        fan_key_name(out, fan, suffixes[i]);
        if (smc_index_has(keys, key_count, out)) {
            return 1;
        }
    }
    return 0;
}

static int parse_rpm(const char *text, long *rpm_out) {
    errno = 0;
    char *end = NULL;
    long rpm = strtol(text, &end, 10);
    if (text[0] == '\0' || end == text || *end != '\0' || errno != 0 || rpm <= 0 || rpm > 20000) {
        fprintf(stderr, "rpm must be a whole number from 1 to 20000\n");
        return 0;
    }
    *rpm_out = rpm;
    return 1;
}

static void print_actuals(unsigned fan_count) {
    for (unsigned fan = 0; fan < fan_count; fan++) {
        char actual_key[5];
        fan_key_name(actual_key, (int)fan, "Ac");
        SmcValue actual = smc_read(g_conn, actual_key);
        if (ok_value(&actual) && strcmp(actual.type, "flt ") == 0 && actual.size >= 4) {
            printf("actual %s %.1f rpm\n", actual_key, smc_decode_flt(actual.bytes));
        }
    }
    fflush(stdout);
}

int anemokey_hold(const char *rpm_text) {
    long rpm = 0;
    if (!parse_rpm(rpm_text, &rpm)) {
        return 1;
    }

    setvbuf(stdout, NULL, _IOLBF, 0);

    int services = 0;
    kern_return_t open_kr = smc_open(&g_conn, &services);
    if (open_kr != KERN_SUCCESS || g_conn == 0) {
        fprintf(stderr, "IOServiceOpen: %s (0x%x)\n", mach_error_string(open_kr), open_kr);
        return smc_is_privilege_error(open_kr) ? 2 : 1;
    }

    uint32_t *keys = NULL;
    size_t key_count = 0;
    uint8_t end_result = 0;
    if (smc_enumerate(g_conn, &keys, &key_count, &end_result) != 0 || end_result != kSmcIndexOutOfRange) {
        fprintf(stderr, "key index read failed\n");
        free(keys);
        smc_close(g_conn);
        g_conn = 0;
        return 1;
    }

    SmcValue count_read = smc_read(g_conn, "FNum");
    if (!ok_value(&count_read) || strcmp(count_read.type, "ui8 ") != 0 || count_read.size < 1) {
        fprintf(stderr, "FNum read failed\n");
        free(keys);
        smc_close(g_conn);
        g_conn = 0;
        return 1;
    }
    unsigned fan_count = count_read.bytes[0];
    if (fan_count == 0 || fan_count > kMaxFans) {
        fprintf(stderr, "FNum value %u is outside 1..%d\n", fan_count, kMaxFans);
        free(keys);
        smc_close(g_conn);
        g_conn = 0;
        return 1;
    }

    char mode_keys[kMaxFans][5];
    float minimums[kMaxFans];
    float maximums[kMaxFans];
    uint8_t original_modes[kMaxFans];
    for (unsigned fan = 0; fan < fan_count; fan++) {
        if (!find_mode_key(keys, key_count, (int)fan, mode_keys[fan])) {
            fprintf(stderr, "fan %u mode key is not in the index\n", fan);
            free(keys);
            smc_close(g_conn);
            g_conn = 0;
            return 1;
        }
        char min_key[5];
        char max_key[5];
        fan_key_name(min_key, (int)fan, "Mn");
        fan_key_name(max_key, (int)fan, "Mx");
        SmcValue minimum = smc_read(g_conn, min_key);
        SmcValue maximum = smc_read(g_conn, max_key);
        SmcValue mode = smc_read(g_conn, mode_keys[fan]);
        if (!ok_value(&minimum) || strcmp(minimum.type, "flt ") != 0 || minimum.size < 4 ||
            !ok_value(&maximum) || strcmp(maximum.type, "flt ") != 0 || maximum.size < 4 ||
            !ok_value(&mode) || strcmp(mode.type, "ui8 ") != 0 || mode.size < 1) {
            fprintf(stderr, "fan %u limit or mode read failed\n", fan);
            free(keys);
            smc_close(g_conn);
            g_conn = 0;
            return 1;
        }
        minimums[fan] = smc_decode_flt(minimum.bytes);
        maximums[fan] = smc_decode_flt(maximum.bytes);
        original_modes[fan] = mode.bytes[0];
        if (!(maximums[fan] > minimums[fan])) {
            fprintf(stderr, "fan %u firmware range is unusable\n", fan);
            free(keys);
            smc_close(g_conn);
            g_conn = 0;
            return 1;
        }
        if ((float)rpm < minimums[fan] || (float)rpm > maximums[fan]) {
            fprintf(stderr, "%ld rpm is outside fan %u firmware range %.0f..%.0f\n",
                    rpm, fan, minimums[fan], maximums[fan]);
            free(keys);
            smc_close(g_conn);
            g_conn = 0;
            return 1;
        }
    }
    free(keys);

    if (geteuid() != 0) {
        fprintf(stderr, "hold requires root. uid %u. not requesting root.\n", getuid());
        smc_close(g_conn);
        g_conn = 0;
        return 2;
    }

    struct sigaction action;
    memset(&action, 0, sizeof action);
    action.sa_handler = on_signal;
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);
    sigaction(SIGHUP, &action, NULL);
    atexit(restore_on_exit);

    g_fan_count = fan_count;
    uint8_t encoded[4];
    smc_encode_flt((float)rpm, encoded);
    uint8_t manual = 1;

    for (unsigned fan = 0; fan < fan_count; fan++) {
        g_fans[fan].engaged = 1;
        memcpy(g_fans[fan].mode_key, mode_keys[fan], 5);
        g_fans[fan].original_mode = original_modes[fan];

        SmcValue mode_write = smc_write(g_conn, mode_keys[fan], &manual, 1);
        if (mode_write.kr != KERN_SUCCESS || mode_write.smc_result != 0) {
            fprintf(stderr, "write %s: %s (0x%x) smc %u\n",
                    mode_keys[fan], mach_error_string(mode_write.kr), mode_write.kr, mode_write.smc_result);
            return smc_is_privilege_error(mode_write.kr) ? 2 : 1;
        }
        SmcValue mode_now = smc_read(g_conn, mode_keys[fan]);
        if (!ok_value(&mode_now) || mode_now.size < 1 || mode_now.bytes[0] != 1) {
            fprintf(stderr, "%s did not stay at manual\n", mode_keys[fan]);
            return 1;
        }

        char target_key[5];
        fan_key_name(target_key, (int)fan, "Tg");
        SmcValue target_write = smc_write(g_conn, target_key, encoded, 4);
        if (target_write.kr != KERN_SUCCESS || target_write.smc_result != 0) {
            fprintf(stderr, "write %s: %s (0x%x) smc %u\n",
                    target_key, mach_error_string(target_write.kr), target_write.kr, target_write.smc_result);
            return smc_is_privilege_error(target_write.kr) ? 2 : 1;
        }
        printf("set %s 1\n", mode_keys[fan]);
        printf("set %s %ld rpm\n", target_key, rpm);
    }

    printf("holding %ld rpm. Ctrl-C restores automatic mode.\n", rpm);
    fflush(stdout);

    for (int i = 0; i < 5 && !g_stop; i++) {
        usleep(200000);
    }
    if (!g_stop) {
        print_actuals(fan_count);
    }
    while (!g_stop) {
        pause();
    }
    return restore_fans() == 0 ? 0 : 1;
}
