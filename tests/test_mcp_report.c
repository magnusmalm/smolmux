/* Regression test for the MCP incident/monitor report builders.
 *
 * The pre-fix builders sized a fixed budget (matching*512 bytes per incident
 * for serial_get_incidents) and appended with
 *     off += snprintf(buf + off, buf_cap - (size_t)off, ...);
 * A single incident serializes to far more than 512 bytes — match_text is
 * char[256] and pre_context is char[SM_ANOMALY_CONTEXT_SIZE]=1024 — so once
 * the accumulated snprintf return values pushed `off` past `buf_cap`,
 * `buf_cap - off` underflowed to a huge size_t and `buf + off` pointed past
 * the allocation: an out-of-bounds heap write (the M15 bug class, at a site
 * M15 never touched). The fix routes these builders through sm_strbuf.
 *
 * This test drives serial_get_incidents (the clearest reproduction) with a
 * worst-case incident store and asserts every incident's tail marker survives
 * in the output — the old builder truncated/overflowed the later incidents. */

#include "test_main.h"
#include "broker.h"
#include "anomaly.h"
#include "sinks/mcp_internal.h"
#include "cJSON.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define N_INCIDENTS 12

static void test_get_incidents_no_overflow(void)
{
    sm_broker_t broker;
    memset(&broker, 0, sizeof(broker));
    sm_anomaly_init(&broker.anomaly);

    sm_anomaly_detector_t *det = &broker.anomaly;
    det->incidents = calloc(N_INCIDENTS, sizeof(sm_anomaly_incident_t));
    ASSERT_NOT_NULL(det->incidents);
    det->incident_cap = N_INCIDENTS;
    det->incident_count = N_INCIDENTS;

    for (int i = 0; i < N_INCIDENTS; i++) {
        sm_anomaly_incident_t *inc = &det->incidents[i];
        snprintf(inc->id, sizeof(inc->id), "inc-%d", i);
        snprintf(inc->pattern_name, sizeof(inc->pattern_name), "PANIC");
        snprintf(inc->severity, sizeof(inc->severity), "critical");
        inc->timestamp = 1000.0 + i;

        /* Max-length match_text */
        memset(inc->match_text, 'M', sizeof(inc->match_text) - 1);
        inc->match_text[sizeof(inc->match_text) - 1] = '\0';

        /* Near-max pre_context ending in a unique marker so truncation of the
         * tail is detectable. */
        memset(inc->pre_context, 'A', sizeof(inc->pre_context) - 1);
        inc->pre_context[sizeof(inc->pre_context) - 1] = '\0';
        char marker[32];
        int mlen = snprintf(marker, sizeof(marker), "<<END%d>>", i);
        memcpy(inc->pre_context + (sizeof(inc->pre_context) - 1) - (size_t)mlen,
               marker, (size_t)mlen);
    }

    sm_mcp_sink_t sink;
    memset(&sink, 0, sizeof(sink));
    sink.broker = &broker;

    /* No "seconds" arg => since_ts stays 0 => every incident matches. */
    cJSON *args = cJSON_CreateObject();
    char *report = mcp_tool_dispatch(&sink, "serial_get_incidents", args, NULL);
    cJSON_Delete(args);

    ASSERT_NOT_NULL(report);
    if (report) {
        /* Every incident's tail marker must survive intact. */
        for (int i = 0; i < N_INCIDENTS; i++) {
            char marker[32];
            snprintf(marker, sizeof(marker), "<<END%d>>", i);
            ASSERT(strstr(report, marker) != NULL,
                   "pre_context tail marker present (no truncation/overflow)");
        }
        ASSERT(strstr(report, "### Incident 1:") != NULL,
               "first incident header present");
        ASSERT(strstr(report, "### Incident 12:") != NULL,
               "last incident header present");
        free(report);
    }

    /* sm_anomaly_destroy frees det->incidents. */
    sm_anomaly_destroy(&broker.anomaly);
}

/* No matching incidents must still produce a valid, freeable string. */
static void test_get_incidents_empty(void)
{
    sm_broker_t broker;
    memset(&broker, 0, sizeof(broker));
    sm_anomaly_init(&broker.anomaly);

    sm_mcp_sink_t sink;
    memset(&sink, 0, sizeof(sink));
    sink.broker = &broker;

    cJSON *args = cJSON_CreateObject();
    char *report = mcp_tool_dispatch(&sink, "serial_get_incidents", args, NULL);
    cJSON_Delete(args);

    ASSERT_NOT_NULL(report);
    if (report) {
        ASSERT(strstr(report, "No anomalies") != NULL,
               "empty store reports no anomalies");
        free(report);
    }

    sm_anomaly_destroy(&broker.anomaly);
}

int main(void)
{
    printf("test_mcp_report\n");
    RUN_TEST(test_get_incidents_no_overflow);
    RUN_TEST(test_get_incidents_empty);
    TEST_REPORT();
}
