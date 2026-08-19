#include "util/keyspec.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>  /* strcasecmp */

int sm_parse_escape_key(const char *spec, uint8_t *out)
{
    if (!spec || !out)
        return -1;

    /* Named: "esc" (case-insensitive). */
    if (strcasecmp(spec, "esc") == 0) {
        *out = 0x1B;
        return 0;
    }

    /* Caret notation: '^' followed by exactly one character. */
    if (spec[0] == '^' && spec[1] != '\0' && spec[2] == '\0') {
        unsigned char c = (unsigned char)spec[1];

        /* ^? is the canonical spelling of DEL (0x7F). */
        if (c == '?') {
            *out = 0x7F;
            return 0;
        }

        /* Letters map case-insensitively (^a == ^A). */
        c = (unsigned char)toupper(c);

        /* Only @A-Z[\]^_ (0x40..0x5F) name control chars 0x00..0x1F. */
        if (c >= 0x40 && c <= 0x5F) {
            *out = (uint8_t)(c ^ 0x40);
            return 0;
        }
    }

    return -1;
}

void sm_format_escape_key(uint8_t ch, char *buf, size_t n)
{
    if (n == 0)
        return;

    if (ch == 0x1B)
        snprintf(buf, n, "ESC");
    else if (ch == 0x7F)
        snprintf(buf, n, "^?");
    else if (ch < 0x20)
        /* Control char -> caret form; 0x1D -> "Ctrl-]", 0x01 -> "Ctrl-A". */
        snprintf(buf, n, "Ctrl-%c", (char)(ch ^ 0x40));
    else
        /* Printable byte (parser never yields these, but stay honest). */
        snprintf(buf, n, "%c", (char)ch);
}
