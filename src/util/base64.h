#ifndef SM_BASE64_H
#define SM_BASE64_H

#include <stddef.h>
#include <stdint.h>

/* Returns malloc'd base64 string. Caller must free. */
char *sm_base64_encode(const uint8_t *data, size_t len);

/* Returns malloc'd decoded bytes. Sets *out_len. Caller must free. */
uint8_t *sm_base64_decode(const char *b64, size_t b64_len, size_t *out_len);

#endif /* SM_BASE64_H */
