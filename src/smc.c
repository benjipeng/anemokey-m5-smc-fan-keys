#include "smc.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

enum {
    kSmcExternalMethod = 2,
    kSmcReadBytes = 5,
    kSmcWriteBytes = 6,
    kSmcReadIndex = 8,
    kSmcReadKeyInfo = 9
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

static uint32_t key_code(const char *name) {
    return ((uint32_t)(uint8_t)name[0] << 24) |
           ((uint32_t)(uint8_t)name[1] << 16) |
           ((uint32_t)(uint8_t)name[2] << 8) |
           (uint32_t)(uint8_t)name[3];
}

static void type_name(uint32_t type, char out[5]) {
    out[0] = (char)((type >> 24) & 0xff);
    out[1] = (char)((type >> 16) & 0xff);
    out[2] = (char)((type >> 8) & 0xff);
    out[3] = (char)(type & 0xff);
    out[4] = '\0';
}

int smc_is_privilege_error(kern_return_t kr) {
    return kr == kIOReturnNotPrivileged || kr == kIOReturnNotPermitted;
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
    return kr;
}

int smc_open(io_connect_t *conn, int *service_count) {
    io_iterator_t iter = 0;
    kern_return_t kr = IOServiceGetMatchingServices(
        kIOMainPortDefault, IOServiceMatching("AppleSMCKeysEndpoint"), &iter);
    if (kr != KERN_SUCCESS) {
        return kr;
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
    if (service_count != NULL) {
        *service_count = services;
    }
    if (services != 1) {
        if (chosen != 0) {
            IOObjectRelease(chosen);
        }
        return kIOReturnNotFound;
    }

    kr = IOServiceOpen(chosen, mach_task_self(), 0, conn);
    IOObjectRelease(chosen);
    return kr;
}

void smc_close(io_connect_t conn) {
    if (conn != 0) {
        IOServiceClose(conn);
    }
}

static SmcValue read_sized(io_connect_t conn, uint32_t key, uint32_t size, const char type[5], uint8_t attr) {
    SmcValue value;
    memset(&value, 0, sizeof value);
    memcpy(value.type, type, 5);
    value.attr = attr;
    value.reported_size = size;
    if (size > sizeof value.bytes) {
        value.size_rejected = 1;
        return value;
    }

    SmcKeyData in;
    SmcKeyData out;
    memset(&in, 0, sizeof in);
    in.key = key;
    in.key_info.data_size = size;
    in.data8 = kSmcReadBytes;
    value.kr = smc_call(conn, &in, &out);
    if (value.kr != KERN_SUCCESS) {
        return value;
    }
    value.smc_result = out.result;
    if (out.result != 0) {
        return value;
    }
    value.size = (uint8_t)size;
    memcpy(value.bytes, out.bytes, size);
    return value;
}

SmcValue smc_read(io_connect_t conn, const char *name) {
    SmcValue value;
    memset(&value, 0, sizeof value);

    SmcKeyData in;
    SmcKeyData out;
    memset(&in, 0, sizeof in);
    in.key = key_code(name);
    in.data8 = kSmcReadKeyInfo;
    value.kr = smc_call(conn, &in, &out);
    if (value.kr != KERN_SUCCESS) {
        return value;
    }
    value.smc_result = out.result;
    if (out.result != 0) {
        return value;
    }

    value.reported_size = out.key_info.data_size;
    value.attr = out.key_info.data_attributes;
    type_name(out.key_info.data_type, value.type);
    return read_sized(conn, in.key, out.key_info.data_size, value.type, value.attr);
}

SmcValue smc_write(io_connect_t conn, const char *name, const uint8_t *bytes, uint32_t size) {
    SmcValue value;
    memset(&value, 0, sizeof value);
    if (size > sizeof value.bytes) {
        value.size_rejected = 1;
        value.reported_size = size;
        value.kr = kIOReturnBadArgument;
        return value;
    }

    SmcKeyData in;
    SmcKeyData out;
    memset(&in, 0, sizeof in);
    in.key = key_code(name);
    in.data8 = kSmcWriteBytes;
    in.key_info.data_size = size;
    memcpy(in.bytes, bytes, size);
    value.kr = smc_call(conn, &in, &out);
    value.smc_result = out.result;
    value.size = (uint8_t)size;
    return value;
}

int smc_enumerate(io_connect_t conn, uint32_t **keys_out, size_t *count_out, uint8_t *end_result) {
    size_t cap = 512;
    size_t count = 0;
    uint32_t *keys = malloc(cap * sizeof *keys);
    if (keys == NULL) {
        return 1;
    }

    for (;;) {
        if (count >= kSmcKeyCap) {
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

int smc_index_has(const uint32_t *keys, size_t count, const char *name) {
    uint32_t code = key_code(name);
    for (size_t i = 0; i < count; i++) {
        if (keys[i] == code) {
            return 1;
        }
    }
    return 0;
}

float smc_decode_flt(const uint8_t *bytes) {
    uint32_t bits = (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
                    ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
    float value = 0;
    memcpy(&value, &bits, sizeof value);
    return value;
}

void smc_encode_flt(float value, uint8_t out[4]) {
    uint32_t bits = 0;
    memcpy(&bits, &value, sizeof bits);
    out[0] = (uint8_t)(bits & 0xff);
    out[1] = (uint8_t)((bits >> 8) & 0xff);
    out[2] = (uint8_t)((bits >> 16) & 0xff);
    out[3] = (uint8_t)((bits >> 24) & 0xff);
}
