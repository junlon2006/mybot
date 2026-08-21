/* SPDX-License-Identifier: Apache-2.0 */
#ifndef MYBOT_AUDIO_BK725X_H_
#define MYBOT_AUDIO_BK725X_H_

/* Convenience umbrella header — includes all audio sub-modules for
 * backward compatibility.  New code should include only the specific
 * sub-module headers it needs. */

#ifdef __cplusplus
extern "C" {
#endif

#include "mybot_audio_capture_bk725x.h"
#include "mybot_audio_playback_bk725x.h"
#include "mybot_audio_shared_bk725x.h"
#include "mybot_audio_volume_bk725x.h"
#include "mybot_audio_power_bk725x.h"

#ifdef __cplusplus
}
#endif

#endif /* MYBOT_AUDIO_BK725X_H_ */
