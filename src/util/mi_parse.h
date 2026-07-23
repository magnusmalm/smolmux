#ifndef SM_MI_PARSE_H
#define SM_MI_PARSE_H

#include <stddef.h>
#include "cJSON.h"

/* GDB/MI output parser. One line of GDB's --interpreter=mi output becomes one
 * record. Grammar (subset of the GDB/MI spec):
 *
 *   output            -> ( out-of-band-record )* [ result-record ] "(gdb)" nl
 *   result-record     -> [token] "^" result-class ( "," result )*
 *   out-of-band       -> [token] ("*"|"+"|"=") async-class ( "," result )*   (async)
 *                      |  ("~"|"@"|"&") c-string                              (stream)
 *   result            -> variable "=" value
 *   value             -> const | tuple | list
 *   tuple             -> "{}" | "{" result ( "," result )* "}"
 *   list              -> "[]" | "[" value (","value)* "]" | "[" result (","result)* "]"
 *
 * This is a client-side parser: smolmux's broker and GDB link stay dumb byte
 * pipes, and the gdb-mcp client runs this over the raw output stream, matching
 * result records to the commands it sent by their leading MI token. */

typedef enum sm_mi_type {
    SM_MI_RESULT,        /* ^  — command result (done/running/connected/error/exit) */
    SM_MI_EXEC_ASYNC,    /* *  — execution state change (*stopped, *running) */
    SM_MI_STATUS_ASYNC,  /* +  — progress (e.g. download) */
    SM_MI_NOTIFY_ASYNC,  /* =  — notification (=thread-group-added, =breakpoint-modified) */
    SM_MI_CONSOLE,       /* ~  — console stream (normal gdb text output) */
    SM_MI_TARGET,        /* @  — target stream (remote target output) */
    SM_MI_LOG,           /* &  — log stream (gdb internal/log text) */
    SM_MI_PROMPT,        /* "(gdb)" prompt line */
    SM_MI_UNKNOWN
} sm_mi_type_t;

typedef struct sm_mi_record {
    sm_mi_type_t type;
    long token;        /* leading MI token, or -1 if none (streams never carry one) */
    char class_[32];   /* result/async class ("done", "stopped", ...); "" for streams */
    cJSON *results;    /* parsed "," results as an object; NULL for stream/prompt records */
    char *stream_data; /* unescaped c-string for CONSOLE/TARGET/LOG; NULL otherwise */
} sm_mi_record_t;

/* Parse one MI line (a trailing \r and/or \n is tolerated) into *out.
 * Returns 0 on success (out populated; free with sm_mi_record_free), or -1 if
 * the line is not a recognizable MI record (out left zeroed, nothing to free). */
int sm_mi_parse_line(const char *line, size_t len, sm_mi_record_t *out);

/* Release the owned members of a record (results cJSON, stream_data). Safe on a
 * zeroed record and idempotent. Does not free the record struct itself. */
void sm_mi_record_free(sm_mi_record_t *rec);

#endif /* SM_MI_PARSE_H */
