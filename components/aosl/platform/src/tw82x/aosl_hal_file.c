#include <sdk.h>
#include <hal/aosl_hal_file.h>

/**
 * @brief 创建目录
 * @param [in] path 目录路径
 * @return 0表示成功，负数表示失败
 * @note 当前平台暂未实现
 */
int aosl_hal_mkdir(const char *path)
{
    (void)path;
    return 0;
}

/**
 * @brief 删除目录
 * @param [in] path 目录路径
 * @return 0表示成功，负数表示失败
 * @note 当前平台暂未实现
 */
int aosl_hal_rmdir(const char *path)
{
    (void)path;
    return 0;
}

/**
 * @brief 检查文件是否存在
 * @param [in] path 文件路径
 * @return 0表示不存在，非零表示存在
 * @note 当前平台暂未实现
 */
int aosl_hal_fexist(const char *path)
{
    (void)path;
    return 0;
}

/**
 * @brief 获取文件大小
 * @param [in] path 文件路径
 * @return >=0表示文件大小，负数表示失败
 * @note 当前平台暂未实现
 */
int aosl_hal_fsize(const char *path)
{
    (void)path;
    return 0;
}

/**
 * @brief 创建文件
 * @param [in] filepath 文件路径
 * @return 0表示成功，负数表示失败
 * @note 当前平台暂未实现
 */
int aosl_hal_file_create(const char *filepath)
{
    (void)filepath;
    return 0;
}

/**
 * @brief 删除文件
 * @param [in] filepath 文件路径
 * @return 0表示成功，负数表示失败
 * @note 当前平台暂未实现
 */
int aosl_hal_file_delete(const char *filepath)
{
    (void)filepath;
    return 0;
}

/**
 * @brief 重命名文件
 * @param [in] old_name 旧文件名
 * @param [in] new_name 新文件名
 * @return 0表示成功，负数表示失败
 * @note 当前平台暂未实现
 */
int aosl_hal_file_rename(const char *old_name, const char *new_name)
{
    (void)old_name;
    (void)new_name;
    return 0;
}

/**
 * @brief 打开文件
 * @param [in] filepath 文件路径
 * @param [in] mode 打开模式
 * @return 文件流句柄，失败返回NULL
 * @note 使用标准C库实现
 */
aosl_fs_t aosl_hal_fopen(const char *filepath, const char *mode)
{
    return (aosl_fs_t)fopen(filepath, mode);
}

/**
 * @brief 关闭文件
 * @param [in] fs 文件流句柄
 * @return 0表示成功，负数表示失败
 * @note 使用标准C库实现
 */
int aosl_hal_fclose(aosl_fs_t fs)
{
    return fclose((FILE *)fs);
}

/**
 * @brief 读取文件
 * @param [in] fs 文件流句柄
 * @param [out] buf 数据缓冲区
 * @param [in] size 要读取的数据大小
 * @return 实际读取的数据大小，失败返回-1
 * @note 使用标准C库实现
 */
int aosl_hal_fread(aosl_fs_t fs, void *buf, size_t size)
{
    size_t ret = fread(buf, 1, size, (FILE *)fs);
    if (ret == 0 && ferror((FILE *)fs)) {
        return -1;
    }
    return (int)ret;
}

/**
 * @brief 写入文件
 * @param [in] fs 文件流句柄
 * @param [in] buf 数据缓冲区
 * @param [in] size 要写入的数据大小
 * @return 实际写入的数据大小，失败返回-1
 * @note 使用标准C库实现
 */
int aosl_hal_fwrite(aosl_fs_t fs, const void *buf, size_t size)
{
    size_t ret = fwrite(buf, 1, size, (FILE *)fs);
    if (ret == 0 && ferror((FILE *)fs)) {
        return -1;
    }
    return (int)ret;
}
