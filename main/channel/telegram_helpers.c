/*
 * ESPClaw - channel/telegram_helpers.c
 * Telegram helper functions implementation.
 * Refactored from zclaw for ESPClaw architecture.
 */
#include "telegram_helpers.h"
#include "config.h"
#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * Chat ID Whitelist Helpers
 * ------------------------------------------------------------------------- */

static bool parse_chat_id_token(const char *start, size_t len, int64_t *out_id)
{
    char token[32];
    char *endptr = NULL;
    long long parsed;

    if (!start || !out_id || len == 0 || len >= sizeof(token)) {
        return false;
    }

    memcpy(token, start, len);
    token[len] = '\0';

    errno = 0;
    parsed = strtoll(token, &endptr, 10);
    if (errno != 0 || !endptr || endptr == token || *endptr != '\0' || parsed == 0) {
        return false;
    }

    *out_id = (int64_t)parsed;
    return true;
}

bool tg_chat_ids_parse(const char *input, int64_t *out_ids, size_t max_ids, size_t *out_count)
{
    const char *cursor = input;
    size_t count = 0;

    if (!input || !out_ids || !out_count || max_ids == 0) {
        return false;
    }

    *out_count = 0;

    while (1) {
        const char *token_start;
        const char *token_end;
        int64_t parsed_id = 0;
        bool duplicate = false;

        while (*cursor != '\0' && isspace((unsigned char)*cursor)) {
            cursor++;
        }

        token_start = cursor;
        while (*cursor != '\0' && *cursor != ',') {
            cursor++;
        }
        token_end = cursor;

        while (token_end > token_start && isspace((unsigned char)token_end[-1])) {
            token_end--;
        }

        if (token_end > token_start) {
            if (!parse_chat_id_token(token_start, (size_t)(token_end - token_start), &parsed_id)) {
                return false;
            }

            for (size_t i = 0; i < count; i++) {
                if (out_ids[i] == parsed_id) {
                    duplicate = true;
                    break;
                }
            }

            if (!duplicate) {
                if (count >= max_ids) {
                    return false;
                }
                out_ids[count++] = parsed_id;
            }
        }

        if (*cursor == '\0') {
            break;
        }
        cursor++;
    }

    if (count == 0) {
        return false;
    }

    *out_count = count;
    return true;
}

bool tg_chat_ids_contains(const int64_t *ids, size_t count, int64_t chat_id)
{
    if (!ids || count == 0 || chat_id == 0) {
        return false;
    }

    for (size_t i = 0; i < count; i++) {
        if (ids[i] == chat_id) {
            return true;
        }
    }
    return false;
}

int64_t tg_chat_ids_resolve_target(const int64_t *ids, size_t count,
                                   int64_t primary_chat_id,
                                   int64_t requested_chat_id)
{
    if (requested_chat_id == 0) {
        return primary_chat_id;
    }

    if (tg_chat_ids_contains(ids, count, requested_chat_id)) {
        return requested_chat_id;
    }

    return 0;
}

/* -------------------------------------------------------------------------
 * Token Helpers
 * ------------------------------------------------------------------------- */

bool tg_extract_bot_id(const char *token, char *out, size_t out_len)
{
    const char *colon = NULL;
    size_t id_len = 0;

    if (!token || !out || out_len == 0) {
        return false;
    }

    out[0] = '\0';

    colon = strchr(token, ':');
    if (!colon || colon == token) {
        return false;
    }

    id_len = (size_t)(colon - token);
    if (id_len + 1 > out_len) {
        return false;
    }

    for (size_t i = 0; i < id_len; i++) {
        if (!isdigit((unsigned char)token[i])) {
            return false;
        }
    }

    memcpy(out, token, id_len);
    out[id_len] = '\0';
    return true;
}

/* -------------------------------------------------------------------------
 * Update ID Extraction (for truncated response recovery)
 * ------------------------------------------------------------------------- */

bool tg_extract_max_update_id(const char *buf, int64_t *max_id_out)
{
    if (!buf || !max_id_out) {
        return false;
    }

    bool found = false;
    int64_t max_id = -1;
    const char *cursor = buf;
    const char *needle = "\"update_id\"";

    while ((cursor = strstr(cursor, needle)) != NULL) {
        const char *colon = strchr(cursor, ':');
        if (!colon) {
            break;
        }

        const char *num_start = colon + 1;
        while (*num_start == ' ' || *num_start == '\t') {
            num_start++;
        }

        char *endptr = NULL;
        long long parsed = strtoll(num_start, &endptr, 10);
        if (endptr != num_start && parsed >= 0) {
            if (!found || parsed > max_id) {
                max_id = parsed;
            }
            found = true;
            cursor = endptr;
        } else {
            cursor = colon + 1;
        }
    }

    if (found) {
        *max_id_out = max_id;
        return true;
    }

    return false;
}
