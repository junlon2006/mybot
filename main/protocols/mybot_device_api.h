#ifndef MYBOT_DEVICE_API_H_
#define MYBOT_DEVICE_API_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ----------------------------------------------------------
 * Device server API — endpoints defined in DEVICE_API.md
 * ---------------------------------------------------------- */

#define MYBOT_DEVICE_API_MAX_URL 256
#define MYBOT_DEVICE_API_MAX_TOKEN 512
#define MYBOT_DEVICE_API_MAX_ID 128

/** Response from POST /devices/pair-codes */
typedef struct {
    char device_id[MYBOT_DEVICE_API_MAX_ID];
    char code[16]; /* 6-digit pair code */
    char pair_token[MYBOT_DEVICE_API_MAX_TOKEN];
    int expires_in_seconds; /* pair code TTL */
    int poll_after_seconds; /* recommended poll interval */
} mybot_device_pair_code_t;

/** Response from GET /devices/{device_id}/binding-status */
typedef struct {
    char status[16]; /* pending | bound | unbound | expired */
    char device_token[MYBOT_DEVICE_API_MAX_TOKEN];
    char agent_id[MYBOT_DEVICE_API_MAX_ID];
    char agent_name[128];
    int poll_after_seconds;
} mybot_device_binding_t;

/** Response from POST /devices/{device_id}/conversations/start */
typedef struct {
    char conversation_id[MYBOT_DEVICE_API_MAX_ID];
    char rtc_app_id[64];
    char rtc_channel[128];
    char rtc_uid[64];       /* string UID assigned by server */
    char rtc_agent_uid[64]; /* string UID of the ConvoAI Agent */
    char rtc_token[MYBOT_DEVICE_API_MAX_TOKEN];
} mybot_device_conversation_t;

/* ----------------------------------------------------------
 * API calls — return 0 on success, a positive HTTP status code for a
 * non-2xx response, or -1 for transport/parsing/local failures.
 * Base URL examples: "http://localhost:3001", "https://mybot.sh3t.agoralab.co/api"
 * ---------------------------------------------------------- */

/** POST /devices/pair-codes (no auth) */
int mybot_device_api_create_pair_code(const char *base_url, const char *device_id,
                                      const char *firmware_ver, const char *hw_model,
                                      mybot_device_pair_code_t *resp);

/** GET /devices/{device_id}/binding-status
 *  auth_header: "Pair <token>" or "Device <token>" */
int mybot_device_api_get_binding_status(const char *base_url, const char *device_id,
                                        const char *auth_header, mybot_device_binding_t *resp);

/** POST /devices/{device_id}/conversations/start
 *  body_params: optional JSON string for extra fields (or NULL). */
int mybot_device_api_start_conversation(const char *base_url, const char *device_id,
                                        const char *device_token, const char *body_params,
                                        mybot_device_conversation_t *resp);

/** POST /devices/{device_id}/conversations/stop */
int mybot_device_api_stop_conversation(const char *base_url, const char *device_id,
                                       const char *device_token, const char *conversation_id,
                                       const char *reason);

#ifdef __cplusplus
}
#endif

#endif /* MYBOT_DEVICE_API_H_ */
