#ifndef SM_SHA1_H
#define SM_SHA1_H

#include <stddef.h>
#include <stdint.h>

#define SM_SHA1_DIGEST_SIZE 20

void sm_sha1(const uint8_t *data, size_t len, uint8_t out[SM_SHA1_DIGEST_SIZE]);

#endif /* SM_SHA1_H */
