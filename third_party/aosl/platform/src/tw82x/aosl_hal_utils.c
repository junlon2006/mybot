#include <sdk.h>

#include <hal/aosl_hal_time.h>
#include <osal/time.h>

/**
 * @brief 生成随机哈希值
 * @param [in] a1 输入参数1
 * @param [in] a2 输入参数2
 * @param [in] a3 输入参数3
 * @return 生成的哈希值
 */
static uint32_t random_a4(uint32_t a1, uint32_t a2, uint32_t a3)
{
    uint32_t hash = a1 ^ 0xffffffff;

    for (int i = 0; i < 4; i++) {
        uint8_t byte = (hash >> (8 * i)) & 0xFF;
        byte ^= (a1 >> (8 * i)) & 0xFF;
        byte ^= (a2 >> (8 * (3 - i))) & 0xFF;
        byte ^= (a3 >> (8 * ((i + 1) % 4))) & 0xFF;
        byte = (byte * 0x1B) ^ (byte >> 1);
        hash ^= (byte << (8 * i));
    }

    hash ^= a2;
    hash ^= hash << 13;
    hash ^= a3;
    hash ^= hash >> 17;

    return hash;
}

/**
 * @brief 获取设备唯一标识
 * @param [out] buf 存储UUID的缓冲区
 * @param [in] buf_sz 缓冲区大小
 * @return 0表示成功，负数表示失败
 * @note 生成至少32个随机字符的唯一标识
 */
int aosl_hal_get_uuid(char buf[], int buf_sz)
{
    if (buf_sz <= 1) {
        return -1;
    }

    uint32_t tick;
    uint32_t r1, r2;
    uint32_t s;
    uint32_t m;

    // 使用固定值作为设备标识（实际平台应使用真实的设备唯一ID）
    m = 0x12345678;

    // 获取系统tick
    tick = (uint32_t)os_mseconds();

    // 获取随机值（使用tick作为种子）
    srand((unsigned int)tick);
    r1 = (uint32_t)rand();
    s = random_a4(tick, r1, m);
    srand(s);
    r2 = (uint32_t)rand();

    snprintf(buf, buf_sz, "%08x%08x%08x%08x", m, tick, r1, r2);
    return 0;
}

/**
 * @brief 获取操作系统版本字符串
 * @param [out] buf 存储版本字符串的缓冲区
 * @param [in] buf_sz 缓冲区大小
 * @return 0表示成功，负数表示失败
 */
int aosl_hal_os_version(char buf[], int buf_sz)
{
    if (buf_sz <= 1) {
        return -1;
    }
    snprintf(buf, buf_sz, "%s", "TW82x RTOS");
    return 0;
}
