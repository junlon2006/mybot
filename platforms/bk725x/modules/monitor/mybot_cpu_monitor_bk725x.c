/* SPDX-License-Identifier: Apache-2.0 */
#include "mybot_cpu_monitor_bk725x.h"

#include "mybot_platform_log.h"

#include <bk_rtos_debug.h>
#include <common/bk_err.h>
#include <common/bk_kernel_err.h>
#include <os/os.h>

#define TAG "mybot_cpu"
#define CPU_MONITOR_INTERVAL_MS 10000
#define CPU_MONITOR_THREAD_PRIORITY 2
#define CPU_MONITOR_THREAD_STACK_SIZE 2048

static beken_thread_t s_monitor_thread;
static beken_semaphore_t s_stop_requested;

static void cpu_monitor_thread(beken_thread_arg_t arg) {
    (void)arg;

    MYBOT_LOGI(TAG, "started: interval=%u ms", CPU_MONITOR_INTERVAL_MS);
    for (;;) {
        bk_err_t result = rtos_get_semaphore(&s_stop_requested, CPU_MONITOR_INTERVAL_MS);
        if (result == BK_OK) {
            break;
        }
        if (result != kTimeoutErr) {
            MYBOT_LOGE(TAG, "stop signal wait failed: %d", result);
            break;
        }

        MYBOT_LOGI(TAG, "dumping system CPU runtime statistics");
        rtos_dump_task_runtime_stats();
    }
    MYBOT_LOGI(TAG, "stopped");
    rtos_delete_thread(NULL);
}

int mybot_cpu_monitor_bk725x_start(void) {
    if (s_monitor_thread) {
        MYBOT_LOGI(TAG, "already started");
        return 0;
    }
    if (rtos_init_semaphore(&s_stop_requested, 1) != BK_OK) {
        MYBOT_LOGE(TAG, "failed to initialize stop signal");
        return -1;
    }

    bk_err_t result = rtos_create_psram_thread(
        &s_monitor_thread, CPU_MONITOR_THREAD_PRIORITY, "mybot_cpu",
        cpu_monitor_thread, CPU_MONITOR_THREAD_STACK_SIZE, NULL);
    if (result != BK_OK) {
        s_monitor_thread = NULL;
        rtos_deinit_semaphore(&s_stop_requested);
        MYBOT_LOGE(TAG, "failed to create monitor thread: %d", result);
        return -1;
    }
    return 0;
}

void mybot_cpu_monitor_bk725x_stop(void) {
    if (!s_monitor_thread) {
        return;
    }

    MYBOT_LOGI(TAG, "stop requested");
    rtos_set_semaphore(&s_stop_requested);
    rtos_thread_join(&s_monitor_thread);
    s_monitor_thread = NULL;
    rtos_deinit_semaphore(&s_stop_requested);
}
