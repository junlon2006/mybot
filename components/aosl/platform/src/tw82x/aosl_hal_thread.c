#include <sdk.h>

#include <api/aosl_mm.h>
#include <api/aosl_log.h>
#include <api/aosl_defs.h>
#include <hal/aosl_hal_thread.h>
#include <osal/task.h>
#include <osal/mutex.h>
#include <osal/semaphore.h>
#include <osal/condv.h>
#include <osal/irq.h>
#include <osal/sleep.h>
#include <osal/string.h>

// 验证静态互斥锁大小是否足够
aosl_static_assert(sizeof(os_mutex_t) <= AOSL_STATIC_MUTEX_SIZE,
                   static_mutex_size_check);

#define AOSL_THREAD_MAX (32)

typedef enum {
    AOSL_THREAD_JOINABLE = 0,
    AOSL_THREAD_DETACHED
} aosl_thread_detach_state_e;

struct aosl_thread_info {
    char *name;
    uint8 inited;
    void *task_hdl;
    os_semaphore_t join_sema;
    aosl_thread_detach_state_e detach_state;
    void *arg;
    void *(*entry)(void *);
};

static struct aosl_thread_info *aosl_threads[AOSL_THREAD_MAX];

static int aosl_thread_set(struct aosl_thread_info *thd)
{
    int32 i = 0;
    uint32 flag = disable_irq();
    for (i = 0; thd && i < AOSL_THREAD_MAX; i++) {
        if (NULL == aosl_threads[i]) {
            aosl_threads[i] = thd;
            break;
        }
    }
    enable_irq(flag);
    return (thd && i < AOSL_THREAD_MAX);
}

static struct aosl_thread_info *aosl_thread_get(aosl_thread_t thread)
{
    int32 i = 0;
    struct aosl_thread_info *thd = NULL;
    uint32 flag = disable_irq();
    for (i = 0; thread && i < AOSL_THREAD_MAX; i++) {
        if (thread == (aosl_thread_t)aosl_threads[i]) {
            thd = aosl_threads[i];
            break;
        }
    }
    enable_irq(flag);
    return thd;
}

static void aosl_thread_del(aosl_thread_t thread)
{
    int32 i = 0;
    uint32 flag = disable_irq();
    for (i = 0; i < AOSL_THREAD_MAX; i++) {
        if (thread == (aosl_thread_t)aosl_threads[i]) {
            aosl_threads[i] = NULL;
            break;
        }
    }
    enable_irq(flag);
}

static void aosl_thread_free(struct aosl_thread_info *thd)
{
    if (!thd) {
        return;
    }
    aosl_thread_del((aosl_thread_t)thd);
    os_sema_del(&thd->join_sema);
    if (thd->name) {
        aosl_free(thd->name);
        thd->name = NULL;
    }
    aosl_free(thd);
}

static void aosl_thread_entry(void *args)
{
    struct aosl_thread_info *thd = (struct aosl_thread_info *)args;
    thd->entry(thd->arg);
    
    while (!thd->inited) {
        os_sleep_ms(5);
    }
    if (thd->detach_state == AOSL_THREAD_JOINABLE) {
        os_sema_up(&thd->join_sema);
    } else {
        aosl_thread_free(thd);
    }
}

static int aosl_thread_priority(aosl_thread_proiority_e priority)
{
    switch (priority) {
        case AOSL_THRD_PRI_LOW:
            return OS_TASK_PRIORITY_LOW;
        case AOSL_THRD_PRI_NORMAL:
            return OS_TASK_PRIORITY_NORMAL;
        case AOSL_THRD_PRI_HIGH:
            return OS_TASK_PRIORITY_HIGH;
        case AOSL_THRD_PRI_HIGHEST:
            return OS_TASK_PRIORITY_HIGH + 0x5;
        case AOSL_THRD_PRI_RT:
            return OS_TASK_PRIORITY_REALTIME;
        default:
            return OS_TASK_PRIORITY_NORMAL;
    }
}

/**
 * @brief 创建线程
 * @param [out] thread 存储创建的线程句柄
 * @param [in] param 线程创建参数
 * @param [in] entry 线程入口函数
 * @param [in] arg 传递给线程的参数
 * @return 0表示成功，负数表示失败
 */
int aosl_hal_thread_create(aosl_thread_t *thread, aosl_thread_param_t *param,
                           void *(*entry)(void *), void *arg)
{
    struct aosl_thread_info *thd = aosl_calloc(1, sizeof(struct aosl_thread_info));
    if (NULL == thd) {
        AOSL_LOG_ERR("thread alloc fail");
        return -1;
    }

    if (!aosl_thread_set(thd)) {
        AOSL_LOG_ERR("no free thread slot, max %d", AOSL_THREAD_MAX);
        aosl_free(thd);
        return -1;
    }

    os_sema_init(&thd->join_sema, 0);
    thd->arg = arg;
    thd->entry = entry;
    
    if (param && param->name) {
        uint32 name_len = os_strlen(param->name) + 1;
        thd->name = aosl_calloc(1, name_len);
        if (thd->name) {
            os_memcpy(thd->name, param->name, name_len);
        }
    } else {
        thd->name = aosl_calloc(1, 16);
        if (thd->name) {
            os_sprintf(thd->name, "aosl_%08x", (uint32)entry);
        }
    }
    if (!thd->name) {
        AOSL_LOG_ERR("thread name alloc fail");
        aosl_thread_del((aosl_thread_t)thd);
        os_sema_del(&thd->join_sema);
        aosl_free(thd);
        return -1;
    }
    thd->detach_state = AOSL_THREAD_JOINABLE;

    uint32 stack_size = (param && param->stack_size > 0) ? param->stack_size : 2048;
    aosl_thread_proiority_e priority = (param) ? param->priority : AOSL_THRD_PRI_NORMAL;
    
    AOSL_LOG_INF("create thread %s, stack_size %d, priority %d start\n", thd->name, stack_size, priority);
    thd->task_hdl = os_task_create(thd->name, aosl_thread_entry, thd,
                                   aosl_thread_priority(priority),
                                   0, NULL, stack_size);
    if (NULL == thd->task_hdl) {
        AOSL_LOG_ERR("task create failed");
        aosl_thread_free(thd);
        return -1;  
    }

    *thread = (aosl_thread_t)thd;
    AOSL_LOG_DBG("thread:%s create !!!", thd->name);
    thd->inited = 1;
    return 0;
}

/**
 * @brief 销毁线程
 * @param [in] thread 线程句柄
 */
void aosl_hal_thread_destroy(aosl_thread_t thread)
{
    struct aosl_thread_info *thd = aosl_thread_get(thread);
    if (thd) {
        AOSL_LOG_DBG("thread:%s destroy !!!", thd->name);
        os_task_destroy(thd->task_hdl);
        aosl_thread_free(thd);
    }
}

/**
 * @brief 退出当前线程
 * @param [in] retval 线程返回值
 */
void aosl_hal_thread_exit(void *retval)
{
    (void)retval;
    struct aosl_thread_info *thd = aosl_thread_get(aosl_hal_thread_self());
    if (thd) {
        AOSL_LOG_DBG("thread:%s exit !!!", thd->name);
        os_task_destroy(thd->task_hdl);
        aosl_thread_free(thd);
    }
}

/**
 * @brief 获取当前线程句柄
 * @return 当前线程句柄
 */
aosl_thread_t aosl_hal_thread_self(void)
{
    void *task_data = os_task_data(os_task_current());
    if (task_data) {
        struct aosl_thread_info *thd = aosl_thread_get((aosl_thread_t)task_data);
        if (thd) {
            AOSL_LOG_DBG("thread:%s self !!!", thd->name);
            return (aosl_thread_t)thd;
        }
    }
    return (aosl_thread_t)os_task_current();
}

/**
 * @brief 设置当前线程名称
 * @param [in] name 线程名称
 * @return 0表示成功，负数表示失败
 * @note 当前平台不支持动态设置线程名称
 */
int aosl_hal_thread_set_name(const char *name)
{
    (void)name;
    return 0;
}

/**
 * @brief 设置当前线程优先级
 * @param [in] priority 线程优先级
 * @return 0表示成功，负数表示失败
 */
int aosl_hal_thread_set_priority(aosl_thread_proiority_e priority)
{
    return 0;
}

/**
 * @brief 等待线程结束
 * @param [in] thread 线程句柄
 * @param [out] retval 存储线程返回值
 * @return 0表示成功，负数表示失败
 */
int aosl_hal_thread_join(aosl_thread_t thread, void **retval)
{
    (void)retval;
    struct aosl_thread_info *thd = aosl_thread_get(thread);
    if (thd) {
        if (thread == aosl_hal_thread_self()) {
            AOSL_LOG_ERR("join self thread");
            return -1;
        }
        if (thd->detach_state == AOSL_THREAD_DETACHED) {
            AOSL_LOG_ERR("join detached thread");
            return -1;
        }

        os_sema_down(&thd->join_sema, -1);
        aosl_thread_free(thd);
    } else {
        AOSL_LOG_ERR("thread not exist");
        return -1;
    }
    return 0;
}

/**
 * @brief 分离线程
 * @param [in] thread 线程句柄
 */
void aosl_hal_thread_detach(aosl_thread_t thread)
{
    return;
}

/**
 * @brief 创建互斥锁
 * @return 互斥锁句柄，失败返回NULL
 */
aosl_mutex_t aosl_hal_mutex_create(void)
{
    os_mutex_t *mutex = aosl_calloc(1, sizeof(os_mutex_t));
    if (NULL == mutex) {
        return NULL;
    }

    int err = os_mutex_init(mutex);
    if (err != 0) {
        AOSL_LOG_ERR("mutex create failed, err=%d", err);
        aosl_free(mutex);
        return NULL;
    }

    return (aosl_mutex_t)mutex;
}

/**
 * @brief 销毁互斥锁
 * @param [in] mutex 互斥锁句柄
 */
void aosl_hal_mutex_destroy(aosl_mutex_t mutex)
{
    os_mutex_t *n_mutex = (os_mutex_t *)mutex;
    if (NULL == n_mutex) {
        return;
    }

    os_mutex_del(n_mutex);
    aosl_free(n_mutex);
}

/**
 * @brief 加锁
 * @param [in] mutex 互斥锁句柄
 * @return 0表示成功，负数表示失败
 */
int aosl_hal_mutex_lock(aosl_mutex_t mutex)
{
    if (!mutex) {
        return -1;
    }
    return os_mutex_lock((os_mutex_t *)mutex, -1);
}

/**
 * @brief 尝试加锁
 * @param [in] mutex 互斥锁句柄
 * @return 0表示成功，负数表示失败
 */
int aosl_hal_mutex_trylock(aosl_mutex_t mutex)
{
    if (!mutex) {
        return -1;
    }
    return os_mutex_lock((os_mutex_t *)mutex, 0);
}

/**
 * @brief 解锁
 * @param [in] mutex 互斥锁句柄
 * @return 0表示成功，负数表示失败
 */
int aosl_hal_mutex_unlock(aosl_mutex_t mutex)
{
    if (!mutex) {
        return -1;
    }
    return os_mutex_unlock((os_mutex_t *)mutex);
}

/**
 * @brief 初始化静态互斥锁
 * @param [in] mutex 静态互斥锁指针
 * @return 0表示成功，负数表示失败
 */
int aosl_hal_static_mutex_init(aosl_static_mutex_t *mutex)
{
    if (!mutex) {
        return -1;
    }

    os_mutex_t *n_mutex = (os_mutex_t *)mutex->opaque;
    return os_mutex_init(n_mutex);
}

/**
 * @brief 清理静态互斥锁
 * @param [in] mutex 静态互斥锁指针
 */
void aosl_hal_static_mutex_fini(aosl_static_mutex_t *mutex)
{
    if (!mutex) {
        return;
    }

    os_mutex_t *n_mutex = (os_mutex_t *)mutex->opaque;
    os_mutex_del(n_mutex);
}

/**
 * @brief 创建条件变量
 * @return 条件变量句柄，失败返回NULL
 */
aosl_cond_t aosl_hal_cond_create(void)
{
    os_condv_t *cond = aosl_calloc(1, sizeof(os_condv_t));
    if (NULL == cond) {
        return NULL;
    }

    int err = os_condv_init(cond);
    if (err != 0) {
        AOSL_LOG_ERR("cond create failed, err=%d", err);
        aosl_free(cond);
        return NULL;
    }

    return (aosl_cond_t)cond;
}

/**
 * @brief 销毁条件变量
 * @param [in] cond 条件变量句柄
 */
void aosl_hal_cond_destroy(aosl_cond_t cond)
{
    os_condv_t *n_cond = (os_condv_t *)cond;
    if (NULL == n_cond) {
        return;
    }

    os_condv_del(n_cond);
    aosl_free(n_cond);
}

/**
 * @brief 发送信号给一个等待线程
 * @param [in] cond 条件变量句柄
 * @return 0表示成功，负数表示失败
 */
int aosl_hal_cond_signal(aosl_cond_t cond)
{
    os_condv_t *n_cond = (os_condv_t *)cond;
    if (NULL == n_cond) {
        return -1;
    }

    return os_condv_signal(n_cond);
}

/**
 * @brief 发送信号给所有等待线程
 * @param [in] cond 条件变量句柄
 * @return 0表示成功，负数表示失败
 */
int aosl_hal_cond_broadcast(aosl_cond_t cond)
{
    os_condv_t *n_cond = (os_condv_t *)cond;
    if (NULL == n_cond) {
        return -1;
    }

    return os_condv_broadcast(n_cond);
}

/**
 * @brief 等待条件变量
 * @param [in] cond 条件变量句柄
 * @param [in] mutex 互斥锁句柄
 * @return 0表示成功，负数表示失败
 */
int aosl_hal_cond_wait(aosl_cond_t cond, aosl_mutex_t mutex)
{
    os_condv_t *n_cond = (os_condv_t *)cond;
    os_mutex_t *n_mutex = (os_mutex_t *)mutex;

    if (NULL == n_cond || NULL == n_mutex) {
        return -1;
    }

    return os_condv_wait(n_cond, n_mutex, -1);
}

/**
 * @brief 带超时的条件变量等待
 * @param [in] cond 条件变量句柄
 * @param [in] mutex 互斥锁句柄
 * @param [in] timeout_ms 超时时间（毫秒）
 * @return 0表示成功，负数表示失败
 */
int aosl_hal_cond_timedwait(aosl_cond_t cond, aosl_mutex_t mutex, intptr_t timeout_ms)
{
    os_condv_t *n_cond = (os_condv_t *)cond;
    os_mutex_t *n_mutex = (os_mutex_t *)mutex;

    if (NULL == n_cond || NULL == n_mutex) {
        return -1;
    }

    return os_condv_wait(n_cond, n_mutex, (uint32)timeout_ms);
}

/**
 * @brief 创建信号量
 * @return 信号量句柄，失败返回NULL
 */
aosl_sem_t aosl_hal_sem_create(void)
{
    os_semaphore_t *sem = aosl_calloc(1, sizeof(os_semaphore_t));
    if (NULL == sem) {
        return NULL;
    }

    int err = os_sema_init(sem, 0);
    if (err != 0) {
        AOSL_LOG_ERR("sem create failed, err=%d", err);
        aosl_free(sem);
        return NULL;
    }

    return (aosl_sem_t)sem;
}

/**
 * @brief 销毁信号量
 * @param [in] sem 信号量句柄
 */
void aosl_hal_sem_destroy(aosl_sem_t sem)
{
    os_semaphore_t *n_sem = (os_semaphore_t *)sem;
    if (NULL == n_sem) {
        return;
    }

    os_sema_del(n_sem);
    aosl_free(n_sem);
}

/**
 * @brief 发送信号（增加信号量计数）
 * @param [in] sem 信号量句柄
 * @return 0表示成功，负数表示失败
 */
int aosl_hal_sem_post(aosl_sem_t sem)
{
    os_semaphore_t *n_sem = (os_semaphore_t *)sem;
    if (NULL == n_sem) {
        return -1;
    }

    return os_sema_up(n_sem);
}

/**
 * @brief 等待信号量（减少信号量计数）
 * @param [in] sem 信号量句柄
 * @return 0表示成功，负数表示失败
 */
int aosl_hal_sem_wait(aosl_sem_t sem)
{
    os_semaphore_t *n_sem = (os_semaphore_t *)sem;
    if (NULL == n_sem) {
        return -1;
    }

    return os_sema_down(n_sem, -1);
}

/**
 * @brief 带超时的信号量等待
 * @param [in] sem 信号量句柄
 * @param [in] timeout_ms 超时时间（毫秒）
 * @return 0表示成功，负数表示失败
 */
int aosl_hal_sem_timedwait(aosl_sem_t sem, intptr_t timeout_ms)
{
    os_semaphore_t *n_sem = (os_semaphore_t *)sem;
    if (NULL == n_sem) {
        return -1;
    }

    return os_sema_down(n_sem, (int32)timeout_ms);
}