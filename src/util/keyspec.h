/*
 * keyspec — parse and format terminal escape/prefix key specifications.
 *
 * Used by smolmux-monitor to let the user choose the escape (prefix) key.
 * Kept as a standalone util so it can be unit-tested independently of the
 * monitor binary (which carries main()).
 */
#ifndef SM_UTIL_KEYSPEC_H
#define SM_UTIL_KEYSPEC_H

#include <stddef.h>
#include <stdint.h>

/*
 * Parse a key specification into a single control byte.
 *
 * Accepted forms (case-insensitive):
 *   "^]", "^A".."^Z", "^\", "^^", "^_", "^?"   caret notation -> control char
 *   "esc"                                       -> 0x1B
 *
 * A prefix key must be a control character (< 0x20 or 0x7F); printable keys
 * are rejected so a normal keystroke can never become the escape.
 *
 * Returns 0 and writes *out on success, -1 on a malformed/unsupported spec.
 */
int sm_parse_escape_key(const char *spec, uint8_t *out);

/*
 * Render a control byte as a human-readable label ("Ctrl-]", "ESC",
 * "Ctrl-A", ...) into buf. Always NUL-terminates when n > 0.
 */
void sm_format_escape_key(uint8_t ch, char *buf, size_t n);

#endif /* SM_UTIL_KEYSPEC_H */
