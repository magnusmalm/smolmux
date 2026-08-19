#ifndef SM_MCP_SINK_H
#define SM_MCP_SINK_H

#include "sink.h"

struct sm_broker;

sm_sink_t *sm_mcp_sink_new(struct sm_broker *broker);

#endif /* SM_MCP_SINK_H */
