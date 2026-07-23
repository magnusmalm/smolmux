#ifndef SM_MCP_SCHEMAS_H
#define SM_MCP_SCHEMAS_H

#include "cJSON.h"

/* Build the MCP `tools/list` result: the serial_* tool schemas. Shared by the
 * in-broker MCP sink (src/sinks/mcp.c) and the standalone smolmux-mcp binary
 * (src/mcp_client.c) so the two MCP surfaces cannot drift. Caller owns the
 * returned array (cJSON_Delete). */
cJSON *sm_mcp_build_tools_list(void);

#endif /* SM_MCP_SCHEMAS_H */
