/* SPDX-License-Identifier: Apache-2.0 */
#include <mybot/platform/mybot_key.h>

#include "linux_backends.h"

#include "api/aosl.h"
#include "api/aosl_atomic.h"
#include "hal/aosl_hal_time.h"

#include <assert.h>
#include <fcntl.h>
#include <unistd.h>

static aosl_atomic_t s_event_count;
static aosl_atomic_t s_last_event;

static void on_key(mybot_key_event_t event, void *user_data) {
    assert(user_data == &s_event_count);
    aosl_atomic_set(&s_last_event, event);
    aosl_atomic_inc(&s_event_count);
}

static void wait_for_event(void) {
    for (int i = 0; i < 1000 && aosl_atomic_read(&s_event_count) == 0; ++i) {
        aosl_hal_msleep(1);
    }
}

static void wait_for_event_count(int count) {
    for (int i = 0; i < 1000 && aosl_atomic_read(&s_event_count) < count; ++i) {
        aosl_hal_msleep(1);
    }
}

int main(void) {
    int pipe_fds[2];
    assert(pipe(pipe_fds) == 0);

    int saved_stdin = dup(STDIN_FILENO);
    assert(saved_stdin >= 0);
    assert(dup2(pipe_fds[0], STDIN_FILENO) == STDIN_FILENO);
    close(pipe_fds[0]);

    aosl_ctor();
    assert(linux_key_platform_register_stdin() == 0);
    assert(mybot_key_service_init(on_key, &s_event_count) == 0);

    assert(write(pipe_fds[1], "p", 1) == 1);
    wait_for_event();
    assert(aosl_atomic_read(&s_event_count) == 1);
    assert(aosl_atomic_read(&s_last_event) == MYBOT_KEY_EVENT_PAIR);

    assert(write(pipe_fds[1], "u", 1) == 1);
    wait_for_event_count(2);
    assert(aosl_atomic_read(&s_event_count) == 2);
    assert(aosl_atomic_read(&s_last_event) == MYBOT_KEY_EVENT_VOLUME_UP);

    assert(write(pipe_fds[1], "d", 1) == 1);
    wait_for_event_count(3);
    assert(aosl_atomic_read(&s_event_count) == 3);
    assert(aosl_atomic_read(&s_last_event) == MYBOT_KEY_EVENT_VOLUME_DOWN);

    mybot_key_service_deinit();
    assert(fcntl(STDIN_FILENO, F_GETFD) >= 0);
    assert(write(pipe_fds[1], "s", 1) == 1);
    aosl_hal_msleep(20);
    assert(aosl_atomic_read(&s_event_count) == 3);

    aosl_dtor();
    close(pipe_fds[1]);
    assert(dup2(saved_stdin, STDIN_FILENO) == STDIN_FILENO);
    close(saved_stdin);
    return 0;
}
