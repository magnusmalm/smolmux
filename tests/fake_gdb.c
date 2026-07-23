/*
 * fake_gdb — a minimal GDB/MI stand-in for the smolmux-gdb-mcp e2e test.
 *
 * Prints the MI banner, then reads token-prefixed MI commands line by line and
 * emits canned `<token>^done,...` result records (plus a couple of async
 * records). Output goes through write() so it is never stdio-buffered — the
 * broker must see each response promptly. Link-level commands sent by the
 * broker's GDB link (`-gdb-set mi-async on`, `-target-select extended-remote`) carry no
 * token and simply get an untokened `^done`.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void emit(const char *s) { (void)!write(1, s, strlen(s)); }

int main(void)
{
    emit("=thread-group-added,id=\"i1\"\n(gdb)\n");

    char line[4096];
    while (fgets(line, sizeof(line), stdin)) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = '\0';

        char tok[32];
        size_t ti = 0;
        char *p = line;
        while (*p >= '0' && *p <= '9' && ti < sizeof(tok) - 1) tok[ti++] = *p++;
        tok[ti] = '\0';
        const char *cmd = p;

        char resp[2048];
        if (strncmp(cmd, "-stack-list-frames", 18) == 0)
            snprintf(resp, sizeof(resp),
                "%s^done,stack=[frame={level=\"0\",addr=\"0x08000abc\","
                "func=\"main\",file=\"main.c\",line=\"42\"}]\n", tok);
        else if (strncmp(cmd, "-data-evaluate-expression", 25) == 0)
            snprintf(resp, sizeof(resp), "%s^done,value=\"42\"\n", tok);
        else if (strncmp(cmd, "-data-list-register-names", 25) == 0)
            snprintf(resp, sizeof(resp),
                "%s^done,register-names=[\"r0\",\"r1\",\"r2\",\"r3\",\"pc\",\"sp\"]\n",
                tok);
        else if (strncmp(cmd, "-data-list-register-values", 26) == 0) {
            /* Values keyed by GDB register number (see register-names above). */
            static const char *const reg_vals[] = {
                "0x0", "0xff", "0x2", "0x3", "0x28c", "0x20008000"
            };
            const char *args = cmd + 26;
            while (*args == ' ') args++;
            /* skip format letter (x / N / r) if present */
            if (*args == 'x' || *args == 'N' || *args == 'r') {
                args++;
                while (*args == ' ') args++;
            }
            char body[1024];
            size_t bo = 0;
            body[0] = '\0';
            int any = 0;
            if (*args) {
                char tmp[256];
                snprintf(tmp, sizeof(tmp), "%s", args);
                char *save = NULL;
                for (char *toki = strtok_r(tmp, " \t", &save); toki;
                     toki = strtok_r(NULL, " \t", &save)) {
                    int n = atoi(toki);
                    if (n < 0 || n >= (int)(sizeof(reg_vals) / sizeof(reg_vals[0])))
                        continue;
                    bo += (size_t)snprintf(body + bo, sizeof(body) - bo,
                        "%s{number=\"%d\",value=\"%s\"}",
                        any ? "," : "", n, reg_vals[n]);
                    any = 1;
                }
            }
            if (!any) {
                /* no index list: return r0 + r1 (legacy default for full dump) */
                snprintf(body, sizeof(body),
                    "{number=\"0\",value=\"0x0\"},{number=\"1\",value=\"0xff\"}");
            }
            snprintf(resp, sizeof(resp),
                "%s^done,register-values=[%s]\n", tok, body);
        }
        else if (strncmp(cmd, "-data-read-memory-bytes", 23) == 0) {
            /* Address-aware answers for gdb_identify_target's probes; contents
             * is a hex byte string in ascending address order (little-endian). */
            if (strstr(cmd, "0xE000ED00"))       /* SCB CPUID = 0x410CC601 (M0+ r0p1) */
                snprintf(resp, sizeof(resp),
                    "%s^done,memory=[{begin=\"0xe000ed00\",contents=\"01c60c41\"}]\n", tok);
            else if (strstr(cmd, "0xE00FF000"))  /* ROM table entry0, ENTRY_PRESENT set */
                snprintf(resp, sizeof(resp),
                    "%s^done,memory=[{begin=\"0xe00ff000\",contents=\"0320f0ff\"}]\n", tok);
            else if (strstr(cmd, "0x41002018"))  /* SAM DSU DID = 0x11010500 (real SAM C21) */
                snprintf(resp, sizeof(resp),
                    "%s^done,memory=[{begin=\"0x41002018\",contents=\"00050111\"}]\n", tok);
            else if (strstr(cmd, "0xE0042000"))
                /* STM32 DBGMCU addr is in the ARM external-PPB region: on the SAM
                 * C21 it reads back as 0 (NOT a fault), so identify must reject a
                 * zero DEV_ID rather than treat it as a valid STM32 match. */
                snprintf(resp, sizeof(resp),
                    "%s^done,memory=[{begin=\"0xe0042000\",contents=\"00000000\"}]\n", tok);
            else if (strstr(cmd, "0x10000100"))
                /* nRF FICR address is unmapped on this (SAM) silicon -> faults */
                snprintf(resp, sizeof(resp),
                    "%s^error,msg=\"Cannot access memory at address\"\n", tok);
            else                                 /* default: CFSR word (fault-register test) */
                snprintf(resp, sizeof(resp),
                    "%s^done,memory=[{begin=\"0xe000ed28\",offset=\"0x0\","
                    "end=\"0xe000ed2c\",contents=\"00000002\"}]\n", tok);
        }
        else if (strncmp(cmd, "-break-insert", 13) == 0)
            snprintf(resp, sizeof(resp),
                "%s^done,bkpt={number=\"1\",addr=\"0x08000abc\",func=\"main\"}\n", tok);
        else if (strncmp(cmd, "-exec-continue", 14) == 0)
            snprintf(resp, sizeof(resp),
                "%s^running\n*running,thread-id=\"all\"\n", tok);
        else if (strncmp(cmd, "-exec-interrupt", 15) == 0)
            snprintf(resp, sizeof(resp),
                "%s^done\n*stopped,reason=\"signal-received\","
                "signal-name=\"SIGINT\",frame={func=\"main\"}\n", tok);
        else if (strncmp(cmd, "-break-list", 11) == 0)
            snprintf(resp, sizeof(resp),
                "%s^done,BreakpointTable={nr_rows=\"0\"}\n", tok);
        else if (strncmp(cmd, "-thread-info", 12) == 0)
            snprintf(resp, sizeof(resp),
                "%s^done,threads=[{id=\"1\",target-id=\"Thread 1\","
                "state=\"stopped\",frame={func=\"main\"}}]\n", tok);
        else
            snprintf(resp, sizeof(resp), "%s^done\n", tok);

        (void)!write(1, resp, strlen(resp));
    }
    return 0;
}
