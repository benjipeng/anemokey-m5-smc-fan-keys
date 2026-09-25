#include "probe.h"
#include "smc.h"

#include <mach/mach_error.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>

static int g_privilege_stop = 0;

static void note_privilege(kern_return_t kr) {
    if (!smc_is_privilege_error(kr) || g_privilege_stop) {
        return;
    }
    g_privilege_stop = 1;
    fprintf(stderr,
            "read requires privilege: %s (0x%x). uid %u. not retrying as root.\n",
            mach_error_string(kr),
            kr,
            getuid());
}

static void print_raw(const uint8_t *bytes, unsigned size) {
    for (unsigned i = 0; i < size; i++) {
        printf("%02x", bytes[i]);
    }
}

/* Fan flt values on this firmware are little-endian IEEE-754.
   Multi-byte integer SMC types stay big-endian. */
static void print_value(const char type[5], const uint8_t *bytes, unsigned size, int as_rpm) {
    if (strcmp(type, "ui8 ") == 0 && size >= 1) {
        printf("value %u", bytes[0]);
        return;
    }
    if (strcmp(type, "ui16") == 0 && size >= 2) {
        unsigned value = ((unsigned)bytes[0] << 8) | bytes[1];
        printf("value %u", value);
        return;
    }
    if (strcmp(type, "ui32") == 0 && size >= 4) {
        unsigned value = ((unsigned)bytes[0] << 24) | ((unsigned)bytes[1] << 16) |
                         ((unsigned)bytes[2] << 8) | bytes[3];
        printf("value %u", value);
        return;
    }
    if (strcmp(type, "flt ") == 0 && size >= 4) {
        if (as_rpm) {
            printf("value %.1f rpm", smc_decode_flt(bytes));
        } else {
            printf("value %.6g", smc_decode_flt(bytes));
        }
        return;
    }
    if (strcmp(type, "fpe2") == 0 && size >= 2) {
        unsigned fixed = ((unsigned)bytes[0] << 8) | bytes[1];
        if (as_rpm) {
            printf("value %.2f rpm", fixed / 4.0);
        } else {
            printf("value %.2f", fixed / 4.0);
        }
        return;
    }
    printf("value undecoded");
}

static void print_smc_result(uint8_t result) {
    if (result == kSmcKeyNotFound) {
        printf("smc_result %u key not found", result);
        return;
    }
    if (result == kSmcIndexOutOfRange) {
        printf("smc_result %u index out of range", result);
        return;
    }
    printf("smc_result %u", result);
}

static int print_read_error(const char *name, const SmcValue *read) {
    printf("%s ", name);
    if (read->kr != KERN_SUCCESS) {
        printf("iokit %s (0x%x)\n", mach_error_string(read->kr), read->kr);
        return 1;
    }
    if (read->size_rejected) {
        printf("key info size %u exceeds 32\n", read->reported_size);
        return 1;
    }
    print_smc_result(read->smc_result);
    printf("\n");
    return read->smc_result == 0 ? 0 : 1;
}

static int print_key(io_connect_t conn, const char *role, const char *name, int as_rpm, SmcValue *out) {
    SmcValue read = smc_read(conn, name);
    note_privilege(read.kr);
    if (out != NULL) {
        *out = read;
    }
    if (g_privilege_stop) {
        return 1;
    }
    if (read.kr != KERN_SUCCESS || read.smc_result != 0 || read.size_rejected) {
        if (role != NULL) {
            printf("%s ", role);
        }
        return print_read_error(name, &read);
    }

    if (role != NULL) {
        printf("%s ", role);
    }
    printf("%s type \"%s\" size %u attr 0x%02x ", name, read.type, read.size, read.attr);
    print_value(read.type, read.bytes, read.size, as_rpm);
    printf(" raw ");
    print_raw(read.bytes, read.size);
    printf("\n");
    return 0;
}

static void fan_key_name(char out[5], int fan, const char *suffix) {
    out[0] = 'F';
    out[1] = (char)('0' + fan);
    out[2] = suffix[0];
    out[3] = suffix[1];
    out[4] = '\0';
}

static int report_missing(io_connect_t conn, const char *name) {
    printf("missing ");
    SmcValue read = smc_read(conn, name);
    note_privilege(read.kr);
    if (g_privilege_stop) {
        return 1;
    }
    return print_read_error(name, &read);
}

int anemokey_probe(void) {
    char model[64] = {0};
    size_t model_len = sizeof model;
    if (sysctlbyname("hw.model", model, &model_len, NULL, 0) != 0) {
        snprintf(model, sizeof model, "unknown");
    }

    printf("model %s\n", model);
    printf("uid %u\n", getuid());
    printf("service AppleSMCKeysEndpoint\n");

    io_connect_t conn = 0;
    int services = 0;
    kern_return_t open_kr = smc_open(&conn, &services);
    printf("service_count %d\n", services);
    if (open_kr != KERN_SUCCESS || conn == 0) {
        if (services != 1 && !smc_is_privilege_error(open_kr)) {
            fprintf(stderr, "expected one AppleSMCKeysEndpoint\n");
        } else {
            fprintf(stderr, "IOServiceOpen: %s (0x%x)\n", mach_error_string(open_kr), open_kr);
        }
        note_privilege(open_kr);
        return g_privilege_stop ? 2 : 1;
    }
    printf("open ok\n");

    uint32_t *keys = NULL;
    size_t key_count = 0;
    uint8_t end_result = 0;
    if (smc_enumerate(conn, &keys, &key_count, &end_result) != 0) {
        fprintf(stderr, "key index read failed\n");
        smc_close(conn);
        return 1;
    }
    printf("keys %zu\n", key_count);
    printf("index_end ");
    print_smc_result(end_result);
    printf("\n");
    int failed = end_result != kSmcIndexOutOfRange;

    unsigned fan_count = 0;
    int have_count = 0;
    if (!failed) {
        SmcValue count_read;
        memset(&count_read, 0, sizeof count_read);
        failed |= print_key(conn, "count", "FNum", 0, &count_read);
        if (!failed && strcmp(count_read.type, "ui8 ") == 0 && count_read.size >= 1) {
            fan_count = count_read.bytes[0];
            have_count = 1;
        } else if (!failed) {
            printf("FNum type \"%s\" is not a one-byte count\n", count_read.type);
            failed = 1;
        }
    }
    if (have_count && fan_count > 9) {
        printf("FNum value %u uses more than one key digit\n", fan_count);
        failed = 1;
        have_count = 0;
    }

    if (have_count) {
        const char *mode_suffixes[] = {"md", "Md", "MD", "mD"};
        for (unsigned fan = 0; fan < fan_count && !g_privilege_stop; fan++) {
            char name[5];
            fan_key_name(name, (int)fan, "Ac");
            failed |= print_key(conn, "actual", name, 1, NULL);
            if (g_privilege_stop) {
                break;
            }
            fan_key_name(name, (int)fan, "Mn");
            failed |= print_key(conn, "minimum", name, 1, NULL);
            if (g_privilege_stop) {
                break;
            }
            fan_key_name(name, (int)fan, "Mx");
            failed |= print_key(conn, "maximum", name, 1, NULL);
            if (g_privilege_stop) {
                break;
            }

            int mode_found = 0;
            for (size_t s = 0; s < sizeof mode_suffixes / sizeof mode_suffixes[0]; s++) {
                fan_key_name(name, (int)fan, mode_suffixes[s]);
                if (!smc_index_has(keys, key_count, name)) {
                    continue;
                }
                failed |= print_key(conn, "mode", name, 0, NULL);
                mode_found = 1;
                if (g_privilege_stop) {
                    break;
                }
            }
            if (!mode_found && !g_privilege_stop) {
                fan_key_name(name, (int)fan, "md");
                failed |= report_missing(conn, name);
                fan_key_name(name, (int)fan, "Md");
                failed |= report_missing(conn, name);
            }
        }
    }

    if (!g_privilege_stop) {
        const char *alternates[] = {"F0Md", "F1Md", "Ftst"};
        for (size_t i = 0; i < sizeof alternates / sizeof alternates[0]; i++) {
            if (smc_index_has(keys, key_count, alternates[i])) {
                failed |= print_key(conn, "present", alternates[i], 0, NULL);
            } else {
                report_missing(conn, alternates[i]);
            }
            if (g_privilege_stop) {
                break;
            }
        }
    }

    free(keys);
    smc_close(conn);
    if (g_privilege_stop) {
        return 2;
    }
    return failed ? 1 : 0;
}
