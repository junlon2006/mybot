#include <sdk.h>

#include <hal/aosl_hal_log.h>

/**
 * @brief 格式化输出函数
 * @param [in] format 格式字符串
 * @param [in] args 可变参数列表
 * @return 输出的字符数，失败返回-1
 * @note 使用vsnprintf格式化输出到标准输出
 */
int aosl_hal_printf(const char *format, va_list args)
{
    char buffer[512];
    int len = vsnprintf(buffer, sizeof(buffer), format, args);
    if (len > 0) {
        printf("%s", buffer);
    }
    return len;
}
