#ifndef SM_JSON_HELPERS_H
#define SM_JSON_HELPERS_H

#include "cJSON.h"

const char *sm_json_get_string(const cJSON *obj, const char *key);
int sm_json_get_int(const cJSON *obj, const char *key, int def);
int sm_json_get_bool(const cJSON *obj, const char *key, int def);
double sm_json_get_double(const cJSON *obj, const char *key, double def);

#endif /* SM_JSON_HELPERS_H */
