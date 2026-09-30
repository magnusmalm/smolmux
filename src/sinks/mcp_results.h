#ifndef SM_MCP_RESULTS_H
#define SM_MCP_RESULTS_H

#include "cJSON.h"

/* Serial MCP tool results that carry structuredContent. Shared by the
 * in-broker MCP sink (src/sinks/mcp_tools.c) and the standalone smolmux-mcp
 * (src/mcp_client.c): both feed the same wire-shaped JSON (status_response,
 * incidents array, history page) in, so prose and structured output cannot
 * drift between the two surfaces.
 *
 * Every *_text function returns malloc'd prose (never NULL). Every
 * *_structured function returns a new object matching the tool's
 * outputSchema, or NULL on allocation failure. */

/* outputSchema for a tool, or NULL if the tool declares none. Caller owns. */
cJSON *sm_mcp_output_schema(const char *tool);
int sm_mcp_tool_has_output_schema(const char *tool);

/* Build a tools/call result: one text block, plus structuredContent when
 * given (takes ownership). isError is set when the text is an [ERROR], or
 * when a tool that declares an outputSchema has no structured value, so a
 * result never claims success without conforming content. tool may be NULL
 * (deferred results for tools with no outputSchema). */
cJSON *sm_mcp_tool_call_result(const char *tool, const char *text,
                               cJSON *structured);

/* serial_port_status from a status_response. last_link_event may be NULL. */
char  *sm_mcp_port_status_text(const cJSON *status, const char *last_link_event);
cJSON *sm_mcp_port_status_structured(const cJSON *status,
                                     const char *last_link_event);

/* serial_boot_status from a status_response (its optional "boot" object). */
char  *sm_mcp_boot_status_text(const cJSON *status);
cJSON *sm_mcp_boot_status_structured(const cJSON *status);

/* serial_get_incidents from an incidents_response "incidents" array
 * (NULL is treated as empty). */
char  *sm_mcp_incidents_text(const cJSON *incidents);
cJSON *sm_mcp_incidents_structured(const cJSON *incidents);

/* serial_list_ports: scans the host once and fills both outputs.
 * *structured may come back NULL on allocation failure. */
char  *sm_mcp_list_ports(cJSON **structured);

/* serial_output_history. Cursor mode: page is {cursor, dropped, has_more,
 * chunks:[{text, timestamp, seq_start}]}; text is the page as JSON (what
 * agents parsed before 0.5.0). Text mode: text is the prose history. */
char  *sm_mcp_history_page_text(const cJSON *page);
cJSON *sm_mcp_history_page_structured(const cJSON *page);
cJSON *sm_mcp_history_text_structured(const char *text);

#endif /* SM_MCP_RESULTS_H */
