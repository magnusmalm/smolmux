#ifndef SM_MCP_SCHEMAS_H
#define SM_MCP_SCHEMAS_H

#include "cJSON.h"

/* Build the MCP `tools/list` result: the serial_* tool schemas. Shared by the
 * in-broker MCP sink (src/sinks/mcp.c) and the standalone smolmux-mcp binary
 * (src/mcp_client.c) so the two MCP surfaces cannot drift. Caller owns the
 * returned array (cJSON_Delete). */
cJSON *sm_mcp_build_tools_list(void);

/* Q-SMOLMUX-03 A: mutate tools (SysRq/pin/TX) are opt-in. */
int sm_mcp_mutate_enabled(void);
int sm_mcp_tool_is_mutate(const char *name);

#endif /* SM_MCP_SCHEMAS_H */
