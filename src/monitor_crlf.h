#ifndef SM_MONITOR_CRLF_H
#define SM_MONITOR_CRLF_H

#include <stddef.h>
#include <stdint.h>

/* The monitor puts the terminal in raw mode (cfmakeraw clears OPOST), so a
 * device that ends lines with a bare "\n" leaves the cursor in its column and
 * the next line starts mid-screen. Map each bare LF to CRLF for display.
 * *prev_cr carries "last byte was CR" across output chunks, so a CRLF split
 * between two chunks is not doubled. out must hold 2 * len bytes. Returns
 * the number of bytes written to out. */
static inline size_t sm_mon_map_lf(const uint8_t *in, size_t len,
                                   uint8_t *out, int *prev_cr)
{
    size_t n = 0;
    for (size_t i = 0; i < len; i++) {
        if (in[i] == '\n' && !*prev_cr)
            out[n++] = '\r';
        out[n++] = in[i];
        *prev_cr = (in[i] == '\r');
    }
    return n;
}

#endif /* SM_MONITOR_CRLF_H */
