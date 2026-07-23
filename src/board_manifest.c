#include "board_manifest.h"
#include "constants.h"
#include "logger.h"
#include "util/sock_util.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LOG_TAG "board"

static void copy_str(char *dst, size_t dlen, cJSON *obj, const char *key)
{
    cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsString(it) && it->valuestring)
        snprintf(dst, dlen, "%s", it->valuestring);
}

int sm_board_manifest_from_json(const char *json, sm_board_manifest_t *out)
{
    memset(out, 0, sizeof(*out));

    cJSON *root = cJSON_Parse(json);
    if (!root) {
        SM_LOG_ERROR(LOG_TAG, "manifest JSON parse failed");
        return -1;
    }

    int rc = -1;
    cJSON *name = cJSON_GetObjectItemCaseSensitive(root, "board");
    if (!cJSON_IsString(name) || !name->valuestring[0]) {
        SM_LOG_ERROR(LOG_TAG, "manifest missing \"board\" name");
        goto done;
    }
    snprintf(out->board, sizeof(out->board), "%s", name->valuestring);

    cJSON *wires = cJSON_GetObjectItemCaseSensitive(root, "wires");
    if (!cJSON_IsArray(wires) || cJSON_GetArraySize(wires) == 0) {
        SM_LOG_ERROR(LOG_TAG, "manifest has no \"wires\"");
        goto done;
    }

    cJSON *w;
    cJSON_ArrayForEach(w, wires) {
        if (!cJSON_IsObject(w)) continue;
        if (out->wire_count >= SM_BOARD_MAX_WIRES) {
            SM_LOG_WARN(LOG_TAG, "manifest exceeds %d wires, truncating",
                        SM_BOARD_MAX_WIRES);
            break;
        }
        sm_board_wire_t *wire = &out->wires[out->wire_count];
        memset(wire, 0, sizeof(*wire));
        wire->baud = SM_DEFAULT_BAUD;
        snprintf(wire->gdb_path, sizeof(wire->gdb_path), "gdb");

        copy_str(wire->role, sizeof(wire->role), w, "role");
        copy_str(wire->link, sizeof(wire->link), w, "link");
        copy_str(wire->device, sizeof(wire->device), w, "device");
        copy_str(wire->gdb_path, sizeof(wire->gdb_path), w, "gdb_path");
        copy_str(wire->target, sizeof(wire->target), w, "target");
        copy_str(wire->profile, sizeof(wire->profile), w, "profile");
        copy_str(wire->socket, sizeof(wire->socket), w, "socket");
        cJSON *baud = cJSON_GetObjectItemCaseSensitive(w, "baud");
        if (cJSON_IsNumber(baud)) wire->baud = baud->valueint;

        if (!wire->role[0]) {
            SM_LOG_ERROR(LOG_TAG, "wire missing \"role\"");
            goto done;
        }
        if (strcmp(wire->link, "uart") != 0 && strcmp(wire->link, "gdb") != 0) {
            SM_LOG_ERROR(LOG_TAG, "wire %s: \"link\" must be uart or gdb",
                         wire->role);
            goto done;
        }
        if (strcmp(wire->link, "uart") == 0 && !wire->device[0]) {
            SM_LOG_ERROR(LOG_TAG, "wire %s: uart link needs \"device\"",
                         wire->role);
            goto done;
        }
        out->wire_count++;
    }

    rc = out->wire_count > 0 ? 0 : -1;

done:
    cJSON_Delete(root);
    return rc;
}

int sm_board_manifest_load(const char *path, sm_board_manifest_t *out)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        SM_LOG_ERROR(LOG_TAG, "cannot open manifest %s", path);
        return -1;
    }
    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return -1; }
    long size = ftell(fp);
    if (size < 0 || size > 1024 * 1024) { fclose(fp); return -1; }
    rewind(fp);

    char *buf = malloc((size_t)size + 1);
    if (!buf) { fclose(fp); return -1; }
    size_t rd = fread(buf, 1, (size_t)size, fp);
    fclose(fp);
    buf[rd] = '\0';

    int rc = sm_board_manifest_from_json(buf, out);
    free(buf);
    return rc;
}

int sm_board_wire_socket(const sm_board_manifest_t *m, const sm_board_wire_t *w,
                         char *out, size_t len)
{
    if (w->socket[0]) {
        if (strlen(w->socket) >= len) return -1;
        snprintf(out, len, "%s", w->socket);
        return 0;
    }
    return sm_derive_board_socket_path(out, len, m->board, w->role);
}
