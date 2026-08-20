#ifndef SM_MCP_EXPLAIN_H
#define SM_MCP_EXPLAIN_H

#include <stddef.h>

/*
 * Agent-facing error hints for serial MCP tool results (Wave 4 / P1c).
 * Pure string mapping — unit-testable without a broker.
 */

/* Return a static hint string for a known error message, or NULL. */
const char *sm_mcp_error_hint(const char *message);

/* Malloc'd tool text: original message plus "\nHint: ..." when mapped.
 * If message is NULL, returns a generic offline/error string.
 * Always returns a non-NULL malloc'd string (or strdup fallback). */
char *sm_mcp_error_with_hint(const char *message);

/* Offline broker guidance (soft-fail). Malloc'd. */
char *sm_mcp_offline_broker_message(void);

/* If text looks like a tool error / timeout / abort, append Hint: line.
 * Takes ownership of text (frees it). Returns new malloc'd string. */
char *sm_mcp_maybe_explain_result(char *text);

/* Append last max_n incidents as a short footer. Takes ownership of text. */
struct sm_anomaly_incident;
char *sm_mcp_append_recent_incidents(char *text,
                                     const struct sm_anomaly_incident *incs,
                                     size_t count, size_t max_n);

#endif /* SM_MCP_EXPLAIN_H */
