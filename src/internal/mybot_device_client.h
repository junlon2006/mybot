/* SPDX-License-Identifier: Apache-2.0 */
#ifndef MYBOT_DEVICE_CLIENT_H_
#define MYBOT_DEVICE_CLIENT_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ----------------------------------------------------------
 * Device service HTTP client
 * ---------------------------------------------------------- */

#define MYBOT_DEVICE_CLIENT_MAX_URL 640
#define MYBOT_DEVICE_CLIENT_MAX_TOKEN 512
#define MYBOT_DEVICE_CLIENT_MAX_ID 128

#define MYBOT_CONVERSATION_STOP_REASON_USER_REQUESTED "user_requested"
#define MYBOT_CONVERSATION_STOP_REASON_DEVICE_HANGUP "device_hangup"
#define MYBOT_CONVERSATION_STOP_REASON_ERROR "error"

/** Pair-code response. */
typedef struct {
    char code[16]; /* 6-digit pair code */
    char pair_token[MYBOT_DEVICE_CLIENT_MAX_TOKEN];
    int poll_after_seconds; /* recommended poll interval */
} mybot_device_pair_code_t;

/** Device-binding response. */
typedef struct {
    char status[16]; /* pending | bound | unbound | expired | failed */
    char device_token[MYBOT_DEVICE_CLIENT_MAX_TOKEN];
    char agent_id[MYBOT_DEVICE_CLIENT_MAX_ID];
    char agent_name[128];
    int poll_after_seconds;
} mybot_device_binding_t;

/** Conversation-start response. */
typedef struct {
    char conversation_id[MYBOT_DEVICE_CLIENT_MAX_ID];
    char rtc_app_id[64];
    char rtc_channel[128];
    char rtc_uid[64];       /* string UID assigned by server */
    char rtc_agent_uid[64]; /* string RTM peer UID assigned by server */
    char rtc_token[MYBOT_DEVICE_CLIENT_MAX_TOKEN];
} mybot_device_conversation_t;

/** RTC-token renewal response. */
typedef struct {
    char rtc_channel[128];
    char rtc_uid[64];
    char rtc_token[MYBOT_DEVICE_CLIENT_MAX_TOKEN];
} mybot_device_rtc_token_t;

/* ----------------------------------------------------------
 * Client calls - return 0 on success, a positive HTTP status code for a
 * non-2xx response, or -1 for transport/parsing/local failures.
 * ---------------------------------------------------------- */

/** Request a pairing code for the device. */
int mybot_device_client_create_pair_code(const char *base_url, const char *device_id,
                                         const char *firmware_ver, const char *hw_model,
                                         mybot_device_pair_code_t *resp);

/** Query the device binding status. */
int mybot_device_client_get_binding_status(const char *base_url, const char *device_id,
                                           const char *auth_header, mybot_device_binding_t *resp);

/** Start a conversation. body_params is a complete JSON request body; pass
 *  NULL to generate one from the build configuration. */
int mybot_device_client_start_conversation(const char *base_url, const char *device_id,
                                           const char *device_token, const char *body_params,
                                           mybot_device_conversation_t *resp);

/** Renew the RTC token for an active channel and local UID. */
int mybot_device_client_renew_rtc_token(const char *base_url, const char *device_id,
                                        const char *device_token, const char *channel,
                                        const char *local_uid, mybot_device_rtc_token_t *resp);

/** Stop a conversation. */
int mybot_device_client_stop_conversation(const char *base_url, const char *device_id,
                                          const char *device_token, const char *conversation_id,
                                          const char *reason);

#ifdef __cplusplus
}
#endif

#endif /* MYBOT_DEVICE_CLIENT_H_ */
