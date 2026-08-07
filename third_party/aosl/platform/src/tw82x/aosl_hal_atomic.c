#include <sdk.h>
#include <hal/aosl_hal_atomic.h>
#include <osal/atomic.h>
#include <osal/irq.h>

/**
 * @brief 原子读取操作
 * @param [in] v 指向原子变量的指针
 * @return 原子变量的当前值
 * @note 使用SDK原子操作实现
 */
intptr_t aosl_hal_atomic_read(const intptr_t *v)
{
    return (intptr_t)atomic_read((atomic_t *)v);
}

/**
 * @brief 原子设置操作
 * @param [in] v 指向原子变量的指针
 * @param [in] i 要设置的值
 * @note 使用SDK原子操作实现
 */
void aosl_hal_atomic_set(intptr_t *v, intptr_t i)
{
    atomic_set((atomic_t *)v, (uint32)i);
}

/**
 * @brief 原子自增操作
 * @param [in] v 指向原子变量的指针
 * @return 自增前的值
 * @note 使用SDK原子操作实现
 */
intptr_t aosl_hal_atomic_inc(intptr_t *v)
{
    uint32 val = atomic_read((atomic_t *)v);
    atomic_inc((atomic_t *)v);
    return (intptr_t)val;
}

/**
 * @brief 原子自减操作
 * @param [in] v 指向原子变量的指针
 * @return 自减前的值
 * @note 使用SDK原子操作实现
 */
intptr_t aosl_hal_atomic_dec(intptr_t *v)
{
    uint32 val = atomic_read((atomic_t *)v);
    atomic_dec((atomic_t *)v);
    return (intptr_t)val;
}

/**
 * @brief 原子加法操作
 * @param [in] i 要加的值
 * @param [in] v 指向原子变量的指针
 * @return 加法后的结果值
 * @note 使用SDK原子操作实现
 */
intptr_t aosl_hal_atomic_add(intptr_t i, intptr_t *v)
{
    atomic_add((atomic_t *)v, (uint32)i);
    return (intptr_t)atomic_read((atomic_t *)v);
}

/**
 * @brief 原子减法操作
 * @param [in] i 要减的值
 * @param [in] v 指向原子变量的指针
 * @return 减法后的结果值
 * @note 使用SDK原子操作实现
 */
intptr_t aosl_hal_atomic_sub(intptr_t i, intptr_t *v)
{
    atomic_sub((atomic_t *)v, (uint32)i);
    return (intptr_t)atomic_read((atomic_t *)v);
}

/**
 * @brief 原子比较并交换操作(CAS)
 * @param [in] v 指向原子变量的指针
 * @param [in] old 期望的旧值
 * @param [in] new 要设置的新值
 * @return 操作前的实际值
 * @note 如果当前值等于old，则设置为new并返回old；否则返回当前值
 */
intptr_t aosl_hal_atomic_cmpxchg(intptr_t *v, intptr_t old, intptr_t new)
{
    return (intptr_t)atomic_cmpxchg((atomic_t *)v, (uint32)old, (uint32)new);
}

/**
 * @brief 原子交换操作
 * @param [in] v 指向原子变量的指针
 * @param [in] new 要设置的新值
 * @return 交换前的旧值
 * @note 使用SDK原子操作实现
 */
intptr_t aosl_hal_atomic_xchg(intptr_t *v, intptr_t new)
{
    uint32 __mask__ = disable_irq();
    uint32 old_val = ((atomic_t *)v)->counter;
    ((atomic_t *)v)->counter = (uint32)new;
    enable_irq(__mask__);
    return (intptr_t)old_val;
}

/**
 * @brief 全内存屏障
 * @note 确保屏障前的所有内存操作完成后才执行屏障后的操作
 */
void aosl_hal_mb(void)
{
    __sync_synchronize();
}

/**
 * @brief 读内存屏障
 * @note 确保屏障前的所有读操作完成后才执行屏障后的读操作
 */
void aosl_hal_rmb(void)
{
    __sync_synchronize();
}

/**
 * @brief 写内存屏障
 * @note 确保屏障前的所有写操作完成后才执行屏障后的写操作
 */
void aosl_hal_wmb(void)
{
    __sync_synchronize();
}