/* SPDX-License-Identifier: Apache-2.0 */
#ifndef MYBOT_DEVICE_STATE_H_
#define MYBOT_DEVICE_STATE_H_

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MYBOT_DEVICE_STATE_UNPROVISIONED,
    MYBOT_DEVICE_STATE_PAIRING,
    MYBOT_DEVICE_STATE_AWAITING_CLAIM,
    MYBOT_DEVICE_STATE_RUNTIME,
    MYBOT_DEVICE_STATE_IN_CONVERSATION,
} mybot_device_state_t;

#ifdef __cplusplus
}
#endif

#endif /* MYBOT_DEVICE_STATE_H_ */
