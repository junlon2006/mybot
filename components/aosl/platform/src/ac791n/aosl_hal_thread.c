#define BOOL_DEFINE_CONFLICT
#include <hal/aosl_hal_thread.h>
#include <hal/aosl_hal_memory.h>
#include <os/os_api.h>
#include <string.h>
#include <stdio.h>

// --- Thread Implementation Context ---
typedef struct {
    void *(*entry)(void *);
    void *arg;
    void *retval;
    OS_SEM join_sem;
    int pid;
    char name[32];
    int detached;
    int reg_idx;
    volatile int ref_cnt;
} aosl_thread_ctx_t;

static void ctx_ref(aosl_thread_ctx_t *ctx)
{
    if (ctx) {
        OS_ENTER_CRITICAL();
        ctx->ref_cnt++;
        OS_EXIT_CRITICAL();
    }
}

static void ctx_unref(aosl_thread_ctx_t *ctx)
{
    if (!ctx) return;
    int count;
    OS_ENTER_CRITICAL();
    count = --ctx->ref_cnt;
    OS_EXIT_CRITICAL();

    if (count == 0) {
        // Free resources when last reference is gone
        // printf("[AOSL-HAL] Finally freeing context %p (%s)\n", ctx, ctx->name);
        os_sem_del(&ctx->join_sem, 1);
        aosl_hal_free(ctx);
    }
}

// --- Global Thread Registry ---
#define MAX_AOSL_THREADS 16
static aosl_thread_ctx_t *g_aosl_threads[MAX_AOSL_THREADS];
static char g_aosl_worker_names[MAX_AOSL_THREADS][32];
static OS_MUTEX g_registry_lock;
static int g_registry_inited = 0;

static void registry_init(void)
{
    if (g_registry_inited) return;

    OS_ENTER_CRITICAL();
    if (!g_registry_inited) {
        os_mutex_create(&g_registry_lock);
        g_registry_inited = 1;
    }
    OS_EXIT_CRITICAL();
}

static void register_thread(aosl_thread_ctx_t *ctx)
{
    registry_init();
    os_mutex_pend(&g_registry_lock, 0);
    for (int i = 0; i < MAX_AOSL_THREADS; i++) {
        if (g_aosl_threads[i] == NULL) {
            g_aosl_threads[i] = ctx;
            snprintf(g_aosl_worker_names[i], 32, "ah_%d", i);
            ctx->reg_idx = i;
            os_mutex_post(&g_registry_lock);
            return;
        }
    }
    os_mutex_post(&g_registry_lock);
}

static void unregister_thread(aosl_thread_ctx_t *ctx)
{
    registry_init();
    os_mutex_pend(&g_registry_lock, 0);
    for (int i = 0; i < MAX_AOSL_THREADS; i++) {
        if (g_aosl_threads[i] == ctx) {
            g_aosl_threads[i] = NULL;
            os_mutex_post(&g_registry_lock);
            return;
        }
    }
    os_mutex_post(&g_registry_lock);
}

static aosl_thread_ctx_t * find_thread_by_name(const char *name)
{
    if (!name || (intptr_t)name < 0x1000) return NULL;
    registry_init();
    os_mutex_pend(&g_registry_lock, 0);
    for (int i = 0; i < MAX_AOSL_THREADS; i++) {
        if (g_aosl_threads[i] && (g_aosl_worker_names[i] == name || strcmp(g_aosl_worker_names[i], name) == 0)) {
            aosl_thread_ctx_t *ctx = g_aosl_threads[i];
            os_mutex_post(&g_registry_lock);
            return ctx;
        }
    }
    os_mutex_post(&g_registry_lock);
    return NULL;
}

// --- Mutex Implementation (Non-recursive via binary semaphore) ---
aosl_mutex_t aosl_hal_mutex_create(void)
{
    OS_SEM *sem = (OS_SEM *)aosl_hal_calloc(1, sizeof(OS_SEM));
    if (!sem) return NULL;

    if (os_sem_create(sem, 1) != 0) {
        aosl_hal_free(sem);
        return NULL;
    }
    return (aosl_mutex_t)sem;
}

void aosl_hal_mutex_destroy(aosl_mutex_t mutex)
{
    if (mutex) {
        os_sem_del((OS_SEM *)mutex, 1);
        aosl_hal_free(mutex);
    }
}

int aosl_hal_mutex_lock(aosl_mutex_t mutex)
{
    if (!mutex) return -1;
    return os_sem_pend((OS_SEM *)mutex, 0); // 0 = Wait forever
}

int aosl_hal_mutex_trylock(aosl_mutex_t mutex)
{
    if (!mutex) return -1;
    return os_sem_accept((OS_SEM *)mutex);
}

int aosl_hal_mutex_unlock(aosl_mutex_t mutex)
{
    if (!mutex) return -1;
    return os_sem_post((OS_SEM *)mutex);
}

// --- Semaphore Implementation ---
aosl_sem_t aosl_hal_sem_create(void)
{
    OS_SEM *sem = (OS_SEM *)aosl_hal_calloc(1, sizeof(OS_SEM));
    if (!sem) return NULL;

    // Initial count 0? Linux impl sets init 0.
    if (os_sem_create(sem, 0) != 0) {
        aosl_hal_free(sem);
        return NULL;
    }
    return (aosl_sem_t)sem;
}

void aosl_hal_sem_destroy(aosl_sem_t sem)
{
    if (sem) {
        os_sem_del((OS_SEM *)sem, 1);
        aosl_hal_free(sem);
    }
}

int aosl_hal_sem_post(aosl_sem_t sem)
{
    if (!sem) return -1;
    return os_sem_post((OS_SEM *)sem);
}

int aosl_hal_sem_wait(aosl_sem_t sem)
{
    if (!sem) return -1;
    return os_sem_pend((OS_SEM *)sem, 0);
}

int aosl_hal_sem_timedwait(aosl_sem_t sem, intptr_t timeout_ms)
{
    if (!sem) return -1;
    // SDK uses ticks (10ms)
    int ticks = timeout_ms / 10;
    if (ticks == 0 && timeout_ms > 0) ticks = 1;
    return os_sem_pend((OS_SEM *)sem, ticks);
}

// --- Condition Variable Simulation ---
typedef struct {
    OS_SEM lock; // Using OS_SEM(1) for internal lock to be consistent
    OS_SEM sem;
    int waiters;
} aosl_cond_impl_t;

aosl_cond_t aosl_hal_cond_create(void)
{
    aosl_cond_impl_t *cond = (aosl_cond_impl_t *)aosl_hal_calloc(1, sizeof(aosl_cond_impl_t));
    if (!cond) return NULL;

    os_sem_create(&cond->lock, 1);
    os_sem_create(&cond->sem, 0);
    cond->waiters = 0;

    return (aosl_cond_t)cond;
}

void aosl_hal_cond_destroy(aosl_cond_t cond)
{
    aosl_cond_impl_t *impl = (aosl_cond_impl_t *)cond;
    if (impl) {
        os_sem_del(&impl->lock, 1);
        os_sem_del(&impl->sem, 1);
        aosl_hal_free(impl);
    }
}

int aosl_hal_cond_signal(aosl_cond_t cond)
{
    aosl_cond_impl_t *impl = (aosl_cond_impl_t *)cond;
    if (!impl) return -1;

    os_sem_pend(&impl->lock, 0);
    if (impl->waiters > 0) {
        os_sem_post(&impl->sem);
    }
    os_sem_post(&impl->lock);
    return 0;
}

int aosl_hal_cond_broadcast(aosl_cond_t cond)
{
    aosl_cond_impl_t *impl = (aosl_cond_impl_t *)cond;
    if (!impl) return -1;

    os_sem_pend(&impl->lock, 0);
    int n = impl->waiters;
    for (int i = 0; i < n; i++) {
        os_sem_post(&impl->sem);
    }
    os_sem_post(&impl->lock);
    return 0;
}

int aosl_hal_cond_wait(aosl_cond_t cond, aosl_mutex_t mutex)
{
    return aosl_hal_cond_timedwait(cond, mutex, 0);
}

int aosl_hal_cond_timedwait(aosl_cond_t cond, aosl_mutex_t mutex, intptr_t timeout_ms)
{
    aosl_cond_impl_t *impl = (aosl_cond_impl_t *)cond;
    if (!impl || !mutex) return -1;

    // Register waiter
    os_sem_pend(&impl->lock, 0);
    impl->waiters++;
    os_sem_post(&impl->lock);

    // Unlock external mutex and wait
    // IMPORTANT: aosl_mutex_t is currently implemented as OS_SEM* in this HAL
    aosl_hal_mutex_unlock(mutex);

    int ticks = 0;
    if (timeout_ms > 0) {
        ticks = timeout_ms / 10;
        if (ticks == 0) ticks = 1;
    }

    int ret = os_sem_pend(&impl->sem, ticks);

    // Re-lock internal lock to update waiters
    os_sem_pend(&impl->lock, 0);
    impl->waiters--;

    if (ret != 0) {
        // If we timed out, check if a signal arrived just now
        if (os_sem_accept(&impl->sem) == 0) {
            ret = 0; // Signal consumed, not a timeout anymore
        }
    }
    os_sem_post(&impl->lock);

    // Re-lock external mutex
    aosl_hal_mutex_lock(mutex);

    return ret;
}

// --- Thread Implementation ---
static void thread_entry_wrapper(void *p)
{
    aosl_thread_ctx_t *ctx = (aosl_thread_ctx_t *)p;
    if (ctx) {
        ctx->retval = ctx->entry(ctx->arg);
        if (!ctx->detached) {
            os_sem_post(&ctx->join_sem);
        }
        ctx_unref(ctx); // Reference from thread entry is gone
    }
}

int aosl_hal_thread_create(aosl_thread_t *thread, aosl_thread_param_t *param,
                           void *(*entry)(void *), void *args)
{
    aosl_thread_ctx_t *ctx = (aosl_thread_ctx_t *)aosl_hal_calloc(1, sizeof(aosl_thread_ctx_t));
    if (!ctx) return -1;

    ctx->entry = entry;
    ctx->arg = args;
    ctx->detached = 0;
    ctx->ref_cnt = 2; // 1 for handle, 1 for thread
    os_sem_create(&ctx->join_sem, 0);

    static int thread_cnt = 0;
    if (param && param->name) snprintf(ctx->name, 32, "ah_%d_%s", ++thread_cnt, param->name);
    else snprintf(ctx->name, 32, "ah_%d", ++thread_cnt);

    register_thread(ctx);

    int prio = 20; // Default priority
    if (param) {
        // Map AOSL prio to SDK prio
        switch(param->priority) {
            case AOSL_THRD_PRI_LOW: prio = 5; break;
            case AOSL_THRD_PRI_NORMAL: prio = 10; break;
            case AOSL_THRD_PRI_HIGH: prio = 20; break;
            case AOSL_THRD_PRI_HIGHEST: prio = 25; break;
            case AOSL_THRD_PRI_RT: prio = 29; break;
            default: prio = 15; break;
        }
    }

    // CRITICAL FIX: Use thread_fork which is the standard SDK API for dynamic tasks.
    // Setting stack size to 4096 (16KB) to provide ample headroom.
    int stack_size = 4096;
    if (param && param->stack_size > 0) {
        stack_size = param->stack_size / 4;
        if (stack_size < 512) stack_size = 512;
    }

    int ret = thread_fork(g_aosl_worker_names[ctx->reg_idx], prio, stack_size, 0, &ctx->pid, thread_entry_wrapper, ctx);
    if (ret != 0) {
        // cleanup
        unregister_thread(ctx);
        os_sem_del(&ctx->join_sem, 1);
        aosl_hal_free(ctx);
        return -1;
    }

    *thread = (aosl_thread_t)ctx;
    return 0;
}

void aosl_hal_thread_destroy(aosl_thread_t thread)
{
    aosl_thread_ctx_t *ctx = (aosl_thread_ctx_t *)thread;
    if (ctx) {
        unregister_thread(ctx);
        ctx_unref(ctx); // Reference from handle is gone
    }
}

void aosl_hal_thread_exit(void *retval)
{
    // Not fully supported to pass retval via exit in this wrapper without ctx access
    // But thread_entry_wrapper handles return from entry function.
    // If called explicitly, we might leak or not set retval correctly.
    // JieLi doesn't have thread_exit API that returns value.
}

int aosl_hal_thread_set_name(const char *name)
{
    // Not supported dynamically
    return 0;
}

int aosl_hal_thread_set_priority(aosl_thread_proiority_e priority)
{
    char *name = (char *)os_current_task();
    int prio = 15;
    switch(priority) {
        case AOSL_THRD_PRI_LOW: prio = 5; break;
        case AOSL_THRD_PRI_NORMAL: prio = 10; break;
        case AOSL_THRD_PRI_HIGH: prio = 20; break;
        case AOSL_THRD_PRI_HIGHEST: prio = 25; break;
        case AOSL_THRD_PRI_RT: prio = 29; break;
    }
    os_task_set_priority(name, prio);
    return 0;
}

int aosl_hal_thread_join(aosl_thread_t thread, void **retval)
{
    aosl_thread_ctx_t *ctx = (aosl_thread_ctx_t *)thread;
    if (!ctx) return -1;

    // printf("[AOSL-HAL] Joining thread %p...\n", ctx);
    os_sem_pend(&ctx->join_sem, 0); // Wait forever

    if (retval) *retval = ctx->retval;
    // printf("[AOSL-HAL] Thread %p joined\n", ctx);

    return 0;
}

void aosl_hal_thread_detach(aosl_thread_t thread)
{
    aosl_thread_ctx_t *ctx = (aosl_thread_ctx_t *)thread;
    if (ctx) {
        ctx->detached = 1;
        // For detached threads, we can't easily free ctx in destroy because destroy might not be called.
        // But AOSL usually calls destroy anyway if it has the handle.
        // If it doesn't, we will leak ctx for detached threads.
        // To be safe on this platform, let's keep it until destroy.
    }
}

aosl_thread_t aosl_hal_thread_self(void)
{
    const char *name = os_current_task();
    aosl_thread_ctx_t *ctx = find_thread_by_name(name);
    if (ctx) return (aosl_thread_t)ctx;

    // Fallback: return the task handle/name itself if not an AOSL managed thread
    return (aosl_thread_t)name;
}
