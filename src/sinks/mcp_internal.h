#ifndef SM_MCP_INTERNAL_H
#define SM_MCP_INTERNAL_H

#include "constants.h"
#include "cJSON.h"

#include <stddef.h>
#include <stdint.h>

struct sm_broker;

typedef struct sm_mcp_pending {
    cJSON *jsonrpc_id;       /* JSON-RPC id — owned copy, NULL if not active */
    char expect_id[16];      /* expect engine id */
    int timeout_mode;        /* if 1, don't prefix with [TIMEOUT] */
    int is_monitor;          /* if 1, format as monitor report */
    double monitor_start;    /* monitor start timestamp */
} sm_mcp_pending_t;

typedef struct sm_mcp_sink {
    struct sm_broker *broker;

    /* Output ring buffer for serial_read */
    uint8_t *output_buf;
    size_t output_len;
    size_t output_cap;

    /* Pending tool calls */
    sm_mcp_pending_t pending[SM_MCP_MAX_PENDING_CALLS];
    int next_expect_seq;

    /* Read buffer for stdin */
    char *read_buf;
    size_t read_len;
    size_t read_cap;

    int initialized;
    int stdin_fd;
} sm_mcp_sink_t;

/* Shared functions between mcp.c and mcp_tools.c */
void mcp_send_result(cJSON *id, cJSON *result);
void mcp_send_tool_result(cJSON *id, const char *text);
sm_mcp_pending_t *mcp_alloc_pending(sm_mcp_sink_t *mcp, cJSON *jsonrpc_id,
                                     const char *expect_id);
void mcp_gen_expect_id(sm_mcp_sink_t *mcp, char *buf, size_t len);
void mcp_buffer_output(sm_mcp_sink_t *mcp, const uint8_t *data, size_t len);
char *mcp_drain_output(sm_mcp_sink_t *mcp);

/* Tool dispatch (defined in mcp_tools.c). The tools/list schema builder now
 * lives in sinks/mcp_schemas.c (sm_mcp_build_tools_list), shared with the
 * standalone smolmux-mcp binary. */
char  *mcp_tool_dispatch(sm_mcp_sink_t *mcp, const char *name, cJSON *args,
                          cJSON *jsonrpc_id);

#endif /* SM_MCP_INTERNAL_H */
