#include "util/json_helpers.h"

const char *sm_json_get_string(const cJSON *obj, const char *key)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsString(item))
        return item->valuestring;
    return NULL;
}

int sm_json_get_int(const cJSON *obj, const char *key, int def)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsNumber(item))
        return item->valueint;
    return def;
}

int sm_json_get_bool(const cJSON *obj, const char *key, int def)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsBool(item))
        return cJSON_IsTrue(item) ? 1 : 0;
    return def;
}

double sm_json_get_double(const cJSON *obj, const char *key, double def)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsNumber(item))
        return item->valuedouble;
    return def;
}
