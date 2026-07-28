#ifndef MYBOT_DEVICE_API_H_
#define MYBOT_DEVICE_API_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ----------------------------------------------------------
 * JSON helpers (lightweight, embedded-oriented)
 * ---------------------------------------------------------- */

/** Build a JSON string for an object with simple fields.
 *  Caller must free returned string via device_api_json_free().
 *  Format: {"key1":"val1","key2":"val2",...}
 *  keys/values are alternating in the varargs, terminated by NULL.
 *  String values are JSON-escaped automatically.
 */
char *device_api_json_build(const char *first_key, ...);

/** Extract a string field value from a JSON string.
 *  Returns pointer to a newly allocated copy, or NULL if not found.
 *  Caller must free. */
char *device_api_json_get_string(const char *json, const char *key);

/** Extract an integer field value from a JSON string. */
int device_api_json_get_int(const char *json, const char *key, int default_val);

/** Free a string returned by any device_api function. */
void device_api_json_free(void *ptr);

/* ----------------------------------------------------------
 * Device server API — endpoints defined in DEVICE_API.md
 * ---------------------------------------------------------- */

#define DEVICE_API_MAX_URL      256
#define DEVICE_API_MAX_TOKEN    512
#define DEVICE_API_MAX_ID      128

/** Response from POST /devices/pair-codes */
typedef struct {
    char device_id[DEVICE_API_MAX_ID];
    char code[16];              /* 6-digit pair code */
    char pair_token[DEVICE_API_MAX_TOKEN];
    int  expires_in_seconds;    /* pair code TTL */
    int  poll_after_seconds;    /* recommended poll interval */
} device_pair_code_t;

/** Response from GET /devices/{device_id}/binding-status */
typedef struct {
    char status[16];            /* pending | bound | unbound | expired */
    char device_token[DEVICE_API_MAX_TOKEN];
    char agent_id[DEVICE_API_MAX_ID];
    char agent_name[128];
    int  poll_after_seconds;
} device_binding_t;

/** Response from POST /devices/{device_id}/conversations/start */
typedef struct {
    char conversation_id[DEVICE_API_MAX_ID];
    char rtc_app_id[64];
    char rtc_channel[128];
    int  rtc_uid;
    int  rtc_agent_uid;
    char rtc_token[DEVICE_API_MAX_TOKEN];
} device_conversation_t;

/* ----------------------------------------------------------
 * API calls — all return 0 on success, -1 on error.
 * Base URL examples: "http://localhost:3001", "https://mybot.sh3t.agoralab.co/api"
 * ---------------------------------------------------------- */

/** POST /devices/pair-codes (no auth) */
int device_api_create_pair_code(const char *base_url,
                                const char *device_id,
                                const char *firmware_ver,
                                const char *hw_model,
                                device_pair_code_t *resp);

/** GET /devices/{device_id}/binding-status
 *  auth_header: "Pair <token>" or "Device <token>" */
int device_api_get_binding_status(const char *base_url,
                                  const char *device_id,
                                  const char *auth_header,
                                  device_binding_t *resp);

/** POST /devices/{device_id}/conversations/start
 *  body_params: optional JSON string for extra fields (or NULL). */
int device_api_start_conversation(const char *base_url,
                                  const char *device_id,
                                  const char *device_token,
                                  const char *body_params,
                                  device_conversation_t *resp);

/** POST /devices/{device_id}/conversations/stop */
int device_api_stop_conversation(const char *base_url,
                                 const char *device_id,
                                 const char *device_token,
                                 const char *conversation_id,
                                 const char *reason);

#ifdef __cplusplus
}
#endif

#endif /* MYBOT_DEVICE_API_H_ */
