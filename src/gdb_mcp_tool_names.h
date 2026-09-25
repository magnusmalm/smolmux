#ifndef SM_GDB_MCP_TOOL_NAMES_H
#define SM_GDB_MCP_TOOL_NAMES_H

/* Single table for gdb-mcp list + dispatch + tests (ACT-035 / A4.10). */
static const char *const sm_gdb_mcp_tool_names[] = {
    "gdb_launch", "gdb_breakpoint", "gdb_delete_breakpoint", "gdb_continue",
    "gdb_interrupt", "gdb_step", "gdb_backtrace", "gdb_read_registers",
    "gdb_read_memory", "gdb_evaluate", "gdb_threads", "gdb_load", "gdb_reset",
    "gdb_status", "gdb_wait_stop", "gdb_console_output",
    "gdb_read_fault_registers", "gdb_read_peripheral", "gdb_identify_target",
    "gdb_generate_profile", "gdb_command",
};

#define SM_GDB_MCP_TOOL_NAME_COUNT \
    ((int)(sizeof(sm_gdb_mcp_tool_names) / sizeof(sm_gdb_mcp_tool_names[0])))

#endif /* SM_GDB_MCP_TOOL_NAMES_H */
