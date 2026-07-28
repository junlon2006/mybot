#include <errno.h>
#include <hal/aosl_hal_errno.h>

/**
 * @brief 将标准errno转换为AOSL HAL错误码
 * @param [in] errnum 系统errno值
 * @return 对应的AOSL HAL错误码
 * @note 根据系统errno值映射到AOSL定义的错误码
 */
int aosl_hal_errno_convert(int errnum)
{
    switch (errnum) {
        case 0:
            return AOSL_HAL_RET_SUCCESS;
        case EAGAIN:
            return AOSL_HAL_RET_EAGAIN;
        case EINTR:
            return AOSL_HAL_RET_EINTR;
        case EINPROGRESS:
            return AOSL_HAL_RET_EINPROGRESS;
        default:
            return AOSL_HAL_RET_EHAL;
    }
}
