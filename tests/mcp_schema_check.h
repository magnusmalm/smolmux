/* Test helper: check an MCP tools/call result against the outputSchema the
 * same server advertised in tools/list. Covers the JSON Schema subset our
 * schemas use (type incl. type arrays, properties, required, items, enum),
 * so a builder that drifts from its schema fails here, not in a client. */
#ifndef SM_TEST_MCP_SCHEMA_CHECK_H
#define SM_TEST_MCP_SCHEMA_CHECK_H

#include "cJSON.h"

#include <stdio.h>
#include <string.h>

static inline int schema_type_ok(const cJSON *v, const char *type)
{
    if (strcmp(type, "object") == 0)  return cJSON_IsObject(v);
    if (strcmp(type, "array") == 0)   return cJSON_IsArray(v);
    if (strcmp(type, "string") == 0)  return cJSON_IsString(v);
    if (strcmp(type, "boolean") == 0) return cJSON_IsBool(v);
    if (strcmp(type, "null") == 0)    return cJSON_IsNull(v);
    if (strcmp(type, "number") == 0)  return cJSON_IsNumber(v);
    if (strcmp(type, "integer") == 0)
        return cJSON_IsNumber(v) && v->valuedouble == (double)(long long)v->valuedouble;
    return 0;
}

/* 0 if v conforms; else -1 with why = "<path>: <reason>". */
static inline int schema_check(const cJSON *v, const cJSON *schema,
                               const char *path, char *why, size_t why_n)
{
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(schema, "type");
    if (cJSON_IsString(type) && !schema_type_ok(v, type->valuestring)) {
        snprintf(why, why_n, "%s: not a %s", path, type->valuestring);
        return -1;
    }
    if (cJSON_IsArray(type)) {
        int ok = 0;
        const cJSON *t;
        cJSON_ArrayForEach(t, type)
            if (cJSON_IsString(t) && schema_type_ok(v, t->valuestring)) ok = 1;
        if (!ok) {
            snprintf(why, why_n, "%s: matches none of its types", path);
            return -1;
        }
    }
    const cJSON *en = cJSON_GetObjectItemCaseSensitive(schema, "enum");
    if (cJSON_IsArray(en)) {
        int ok = 0;
        const cJSON *e;
        cJSON_ArrayForEach(e, en)
            if (cJSON_Compare(e, v, 1)) ok = 1;
        if (!ok) {
            snprintf(why, why_n, "%s: not in enum", path);
            return -1;
        }
    }
    const cJSON *req = cJSON_GetObjectItemCaseSensitive(schema, "required");
    const cJSON *r;
    cJSON_ArrayForEach(r, req) {
        if (!cJSON_GetObjectItemCaseSensitive(v, r->valuestring)) {
            snprintf(why, why_n, "%s: missing required '%s'", path, r->valuestring);
            return -1;
        }
    }
    const cJSON *props = cJSON_GetObjectItemCaseSensitive(schema, "properties");
    const cJSON *p;
    cJSON_ArrayForEach(p, props) {
        const cJSON *child = cJSON_GetObjectItemCaseSensitive(v, p->string);
        if (!child) continue;
        char sub[256];
        snprintf(sub, sizeof(sub), "%s.%s", path, p->string);
        if (schema_check(child, p, sub, why, why_n) != 0) return -1;
    }
    const cJSON *items = cJSON_GetObjectItemCaseSensitive(schema, "items");
    if (items && cJSON_IsArray(v)) {
        int i = 0;
        const cJSON *it;
        cJSON_ArrayForEach(it, v) {
            char sub[256];
            snprintf(sub, sizeof(sub), "%s[%d]", path, i++);
            if (schema_check(it, items, sub, why, why_n) != 0) return -1;
        }
    }
    return 0;
}

/* The outputSchema tools/list gave for name, or NULL. */
static inline const cJSON *tools_output_schema(const cJSON *tools,
                                               const char *name)
{
    const cJSON *t;
    cJSON_ArrayForEach(t, tools) {
        const cJSON *n = cJSON_GetObjectItemCaseSensitive(t, "name");
        if (cJSON_IsString(n) && strcmp(n->valuestring, name) == 0)
            return cJSON_GetObjectItemCaseSensitive(t, "outputSchema");
    }
    return NULL;
}

static inline int tools_output_schema_count(const cJSON *tools)
{
    int n = 0;
    const cJSON *t;
    cJSON_ArrayForEach(t, tools)
        if (cJSON_GetObjectItemCaseSensitive(t, "outputSchema")) n++;
    return n;
}

#endif /* SM_TEST_MCP_SCHEMA_CHECK_H */
