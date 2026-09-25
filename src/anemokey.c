/* Read Apple SMC fan keys through AppleSMCKeysEndpoint.

   Userspace opens that service's AppleSMCClient and calls external method 2
   with an 80-byte SmcKeyData. This program sends only read commands:
   key-by-index (8), key info (9), and key bytes (5).
*/

#include <IOKit/IOKitLib.h>

#include <mach/mach_error.h>

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>

enum {
    kSmcExternalMethod = 2,
    kSmcReadBytes = 5,
    kSmcReadIndex = 8,
    kSmcReadKeyInfo = 9,
    kSmcIndexOutOfRange = 184,
    kSmcKeyNotFound = 132,
    kSmcKeyCap = 10000
};

typedef struct {
    uint8_t major;
    uint8_t minor;
    uint8_t build;
    uint8_t reserved;
    uint16_t release;
} SmcVers;

typedef struct {
    uint16_t version;
    uint16_t length;
    uint32_t cpu_p_limit;
    uint32_t gpu_p_limit;
    uint32_t mem_p_limit;
} SmcPLimit;

typedef struct {
    uint32_t data_size;
    uint32_t data_type;
    uint8_t data_attributes;
} SmcKeyInfo;

typedef struct {
    uint32_t key;
    SmcVers vers;
    SmcPLimit p_limit;
    SmcKeyInfo key_info;
    uint8_t result;
    uint8_t status;
    uint8_t data8;
    uint32_t data32;
    uint8_t bytes[32];
} SmcKeyData;

_Static_assert(sizeof(SmcKeyData) == 80, "AppleSMCClient expects an 80-byte struct");
_Static_assert(offsetof(SmcKeyData, key_info) == 28, "key info offset");
_Static_assert(offsetof(SmcKeyData, data8) == 42, "command offset");
_Static_assert(offsetof(SmcKeyData, data32) == 44, "data32 offset");
_Static_assert(offsetof(SmcKeyData, bytes) == 48, "bytes offset");
_Static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "flt decode uses host little-endian");

typedef struct {
    kern_return_t kr;
    uint8_t smc_result;
    int size_rejected;
    uint32_t reported_size;
    char type[5];
    uint8_t size;
    uint8_t attr;
    uint8_t bytes[32];
} SmcRead;

static int g_privilege_stop = 0;

static uint32_t key_code(const char name[4]) {
    return ((uint32_t)(uint8_t)name[0] << 24) |
           ((uint32_t)(uint8_t)name[1] << 16) |
           ((uint32_t)(uint8_t)name[2] << 8) |
           (uint32_t)(uint8_t)name[3];
}

static void key_name(uint32_t key, char out[5]) {
    out[0] = (char)((key >> 24) & 0xff);
    out[1] = (char)((key >> 16) & 0xff);
    out[2] = (char)((key >> 8) & 0xff);
    out[3] = (char)(key & 0xff);
    out[4] = '\0';
}

static void type_name(uint32_t type, char out[5]) {
    key_name(type, out);
}

static int privilege_error(kern_return_t kr) {
    return kr == kIOReturnNotPrivileged || kr == kIOReturnNotPermitted;
}

static void note_privilege(kern_return_t kr) {
    if (!privilege_error(kr)) {
        return;
    }
    g_privilege_stop = 1;
    fprintf(stderr,
            "read requires privilege: %s (0x%x). uid %u. not retrying as root.\n",
            mach_error_string(kr),
            kr,
            getuid());
}

static kern_return_t smc_call(io_connect_t conn, const SmcKeyData *input, SmcKeyData *output) {
    SmcKeyData in = *input;
    size_t out_size = sizeof *output;
    memset(output, 0, sizeof *output);
    kern_return_t kr = IOConnectCallStructMethod(
        conn, kSmcExternalMethod, &in, sizeof in, output, &out_size);
    if (kr == KERN_SUCCESS && out_size != sizeof *output) {
        return kIOReturnBadArgument;
    }
    note_privilege(kr);
    return kr;
}

static SmcRead read_key(io_connect_t conn, uint32_t key) {
    SmcRead read;
    memset(&read, 0, sizeof read);

    SmcKeyData in;
    SmcKeyData out;
    memset(&in, 0, sizeof in);
    in.key = key;
    in.data8 = kSmcReadKeyInfo;
    read.kr = smc_call(conn, &in, &out);
    if (read.kr != KERN_SUCCESS) {
        return read;
    }
    read.smc_result = out.result;
    if (out.result != 0) {
        return read;
    }

    read.reported_size = out.key_info.data_size;
    read.attr = out.key_info.data_attributes;
    type_name(out.key_info.data_type, read.type);
    if (out.key_info.data_size > sizeof read.bytes) {
        read.size_rejected = 1;
        return read;
    }

    uint32_t size = out.key_info.data_size;
    memset(&in, 0, sizeof in);
    in.key = key;
    in.key_info.data_size = size;
    in.data8 = kSmcReadBytes;
    read.kr = smc_call(conn, &in, &out);
    if (read.kr != KERN_SUCCESS) {
        return read;
    }
    read.smc_result = out.result;
    if (out.result != 0) {
        return read;
    }
    read.size = (uint8_t)size;
    memcpy(read.bytes, out.bytes, size);
    return read;
}

static int key_in_index(const uint32_t *keys, size_t count, const char *name) {
    uint32_t code = key_code(name);
    for (size_t i = 0; i < count; i++) {
        if (keys[i] == code) {
            return 1;
        }
    }
    return 0;
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
        uint32_t bits = (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
                        ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
        float value = 0;
        memcpy(&value, &bits, sizeof value);
        if (as_rpm) {
            printf("value %.1f rpm", value);
        } else {
            printf("value %.6g", value);
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

static int print_read_error(const char *name, const SmcRead *read) {
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

static int print_key(io_connect_t conn, const char *role, const char *name, int as_rpm, SmcRead *out) {
    SmcRead read = read_key(conn, key_code(name));
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

static int enumerate_keys(io_connect_t conn, uint32_t **keys_out, size_t *count_out, uint8_t *end_result) {
    size_t cap = 512;
    size_t count = 0;
    uint32_t *keys = malloc(cap * sizeof *keys);
    if (keys == NULL) {
        return 1;
    }

    for (;;) {
        if (count >= kSmcKeyCap) {
            fprintf(stderr, "key index exceeded %d entries\n", kSmcKeyCap);
            free(keys);
            return 1;
        }
        SmcKeyData in;
        SmcKeyData out;
        memset(&in, 0, sizeof in);
        in.data8 = kSmcReadIndex;
        in.data32 = (uint32_t)count;
        kern_return_t kr = smc_call(conn, &in, &out);
        if (kr != KERN_SUCCESS) {
            fprintf(stderr, "read index %zu: %s (0x%x)\n", count, mach_error_string(kr), kr);
            free(keys);
            return 1;
        }
        if (out.result != 0) {
            *end_result = out.result;
            break;
        }
        if (count == cap) {
            size_t next = cap * 2;
            uint32_t *grown = realloc(keys, next * sizeof *keys);
            if (grown == NULL) {
                free(keys);
                return 1;
            }
            keys = grown;
            cap = next;
        }
        keys[count++] = out.key;
    }

    *keys_out = keys;
    *count_out = count;
    return 0;
}

static io_connect_t open_endpoint(void) {
    io_iterator_t iter = 0;
    kern_return_t kr = IOServiceGetMatchingServices(
        kIOMainPortDefault, IOServiceMatching("AppleSMCKeysEndpoint"), &iter);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "IOServiceGetMatchingServices: %s (0x%x)\n", mach_error_string(kr), kr);
        note_privilege(kr);
        return 0;
    }

    io_service_t chosen = 0;
    int services = 0;
    io_service_t service = 0;
    while ((service = IOIteratorNext(iter)) != 0) {
        services++;
        if (services == 1) {
            chosen = service;
        } else {
            IOObjectRelease(service);
        }
    }
    IOObjectRelease(iter);
    printf("service_count %d\n", services);
    if (services != 1) {
        fprintf(stderr, "expected one AppleSMCKeysEndpoint\n");
        if (chosen != 0) {
            IOObjectRelease(chosen);
        }
        return 0;
    }

    io_connect_t conn = 0;
    kr = IOServiceOpen(chosen, mach_task_self(), 0, &conn);
    IOObjectRelease(chosen);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "IOServiceOpen: %s (0x%x)\n", mach_error_string(kr), kr);
        note_privilege(kr);
        return 0;
    }
    printf("open ok\n");
    return conn;
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
    SmcRead read = read_key(conn, key_code(name));
    if (g_privilege_stop) {
        return 1;
    }
    return print_read_error(name, &read);
}

int main(void) {
    char model[64] = {0};
    size_t model_len = sizeof model;
    if (sysctlbyname("hw.model", model, &model_len, NULL, 0) != 0) {
        snprintf(model, sizeof model, "unknown");
    }

    printf("model %s\n", model);
    printf("uid %u\n", getuid());
    printf("service AppleSMCKeysEndpoint\n");

    io_connect_t conn = open_endpoint();
    if (conn == 0 || g_privilege_stop) {
        return g_privilege_stop ? 2 : 1;
    }

    uint32_t *keys = NULL;
    size_t key_count = 0;
    uint8_t end_result = 0;
    if (enumerate_keys(conn, &keys, &key_count, &end_result) != 0 || g_privilege_stop) {
        IOServiceClose(conn);
        return g_privilege_stop ? 2 : 1;
    }
    printf("keys %zu\n", key_count);
    printf("index_end ");
    print_smc_result(end_result);
    printf("\n");
    int failed = end_result != kSmcIndexOutOfRange;

    unsigned fan_count = 0;
    int have_count = 0;
    if (!failed) {
        SmcRead count_read;
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
                if (!key_in_index(keys, key_count, name)) {
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
            if (key_in_index(keys, key_count, alternates[i])) {
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
    IOServiceClose(conn);
    if (g_privilege_stop) {
        return 2;
    }
    return failed ? 1 : 0;
}
