#ifndef ANEMOKEY_SMC_H
#define ANEMOKEY_SMC_H

#include <IOKit/IOKitLib.h>

#include <stdint.h>

enum {
    kSmcIndexOutOfRange = 184,
    kSmcKeyNotFound = 132,
    kSmcKeyCap = 10000
};

typedef struct {
    kern_return_t kr;
    uint8_t smc_result;
    int size_rejected;
    uint32_t reported_size;
    char type[5];
    uint8_t size;
    uint8_t attr;
    uint8_t bytes[32];
} SmcValue;

int smc_open(io_connect_t *conn, int *service_count);
void smc_close(io_connect_t conn);
int smc_is_privilege_error(kern_return_t kr);

SmcValue smc_read(io_connect_t conn, const char *name);
SmcValue smc_write(io_connect_t conn, const char *name, const uint8_t *bytes, uint32_t size);
int smc_enumerate(io_connect_t conn, uint32_t **keys_out, size_t *count_out, uint8_t *end_result);
int smc_index_has(const uint32_t *keys, size_t count, const char *name);

float smc_decode_flt(const uint8_t *bytes);
void smc_encode_flt(float value, uint8_t out[4]);

#endif
