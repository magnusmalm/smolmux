#ifndef SM_MCP_INSTRUCTIONS_H
#define SM_MCP_INSTRUCTIONS_H

#include <stddef.h>
#include "cJSON.h"

/*
 * Shared serial-MCP agent guidance (initialize instructions + prompts).
 *
 * Rationale: both smolmux-mcp (standalone) and the in-broker MCP sink must
 * teach the same workflow; one source of truth prevents drift. Text is short
 * because it lands in the client context on every session.
 */

/* Static string for MCP initialize.result.instructions. Never free. */
const char *sm_mcp_serial_instructions(void);

/* MCP prompts/list result object (caller owns; free with cJSON_Delete).
 * Shape: { "prompts": [ { name, description, arguments }, ... ] } */
cJSON *sm_mcp_serial_prompts_list_result(void);

/* MCP prompts/get: on success returns 0 and sets *out_text (malloc'd) and
 * *out_desc (static). On unknown name returns -1. args may be NULL. */
int sm_mcp_serial_prompt_get(const char *name, const cJSON *args,
                             char **out_text, const char **out_desc);

/* Exact prompt count / names for tests (set equality, not >= 1). */
#define SM_MCP_SERIAL_PROMPT_COUNT 3
const char *const *sm_mcp_serial_prompt_names(size_t *count);

#endif /* SM_MCP_INSTRUCTIONS_H */
