/*
 * ESPClaw - channel/telegram_helpers.h
 * Telegram helper functions for chat ID parsing, token extraction, and update parsing.
 * Refactored from zclaw for ESPClaw architecture.
 */
#ifndef TELEGRAM_HELPERS_H
#define TELEGRAM_HELPERS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Maximum allowed chat IDs in whitelist */
#define TG_MAX_CHAT_IDS TELEGRAM_MAX_ALLOWED_CHAT_IDS

/* -------------------------------------------------------------------------
 * Chat ID Whitelist Helpers
 * ------------------------------------------------------------------------- */

/**
 * Parse a comma-separated chat ID list (or single ID) into out_ids.
 * Returns true when at least one valid ID was parsed.
 * Handles whitespace and skips duplicates.
 */
bool tg_chat_ids_parse(const char *input, int64_t *out_ids, size_t max_ids, size_t *out_count);

/**
 * Check if chat_id is present in the provided list.
 * Returns false if list is empty or chat_id is 0.
 */
bool tg_chat_ids_contains(const int64_t *ids, size_t count, int64_t chat_id);

/**
 * Resolve outbound target chat routing.
 * - requested_chat_id == 0: use primary_chat_id
 * - requested_chat_id is in allowed list: use requested_chat_id
 * - requested_chat_id unauthorized: return 0 (drop)
 */
int64_t tg_chat_ids_resolve_target(const int64_t *ids, size_t count,
                                   int64_t primary_chat_id,
                                   int64_t requested_chat_id);

/* -------------------------------------------------------------------------
 * Token Helpers
 * ------------------------------------------------------------------------- */

/**
 * Extract the numeric bot ID prefix from a Telegram token ("<bot_id>:<secret>").
 * Returns false if token format is invalid or output buffer is too small.
 * Used for safe logging without exposing the secret.
 */
bool tg_extract_bot_id(const char *token, char *out, size_t out_len);

/* -------------------------------------------------------------------------
 * Update ID Extraction (for truncated response recovery)
 * ------------------------------------------------------------------------- */

/**
 * Best-effort parser for recovering the max update_id from partially received JSON.
 * Scans for "update_id":<number> patterns without full JSON parsing.
 * Returns true and sets max_id_out when at least one non-negative update_id is found.
 */
bool tg_extract_max_update_id(const char *buf, int64_t *max_id_out);

#endif /* TELEGRAM_HELPERS_H */
