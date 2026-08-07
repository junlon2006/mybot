#include <hal/aosl_hal_file.h>
#include <string.h>
#include <stdlib.h>
#include <fs/fs.h>

static void sanitize_mode(const char *in, char *out) {
    int i = 0, j = 0;
    while(in[i] && j < 7) {
        if(in[i] != 'b') {
            out[j++] = in[i];
        }
        i++;
    }
    out[j] = 0;
}

static char *get_basename(const char *path) {
    const char *p = strrchr(path, '/');
    if (p) return (char*)(p + 1);
    return (char*)path;
}

int aosl_hal_mkdir(const char *path)
{
    // SDK API: int fmk_dir(const char *path, char *folder, u8 mode);
    if(!path || !strlen(path)) return -1;

    // Check if path ends with /
    int len = strlen(path);
    char *cleanPath = (char*)path;
    int needsFree = 0;

    if (len > 1 && path[len-1] == '/') {
        cleanPath = (char*)malloc(len);
        if(!cleanPath) return -1;
        memcpy(cleanPath, path, len-1);
        cleanPath[len-1] = 0;
        needsFree = 1;
    }

    char *p = strrchr(cleanPath, '/');
    if(!p) {
        // Assume root
        int ret = fmk_dir("/", (char*)cleanPath, 0);
        if(needsFree) free(cleanPath);
        return ret;
    }

    int pLen = p - cleanPath;
    char *parent = (char*)malloc(pLen + 1);
    if(!parent) {
        if(needsFree) free(cleanPath);
        return -1;
    }

    if(pLen == 0) strcpy(parent, "/");
    else {
        memcpy(parent, cleanPath, pLen);
        parent[pLen] = 0;
    }

    char *folderName = p + 1;
    // fmk_dir requires folder name to have /? based on header comment "例如 /test"
    int fLen = strlen(folderName);
    char *folderArg = (char*)malloc(fLen + 2);
    if(!folderArg) {
        free(parent);
        if(needsFree) free(cleanPath);
        return -1;
    }

    if(folderName[0] != '/') {
        folderArg[0] = '/';
        strcpy(folderArg+1, folderName);
    } else {
        strcpy(folderArg, folderName);
    }

    int ret = fmk_dir(parent, folderArg, 0);

    free(parent);
    free(folderArg);
    if(needsFree) free(cleanPath);
    return ret;
}

int aosl_hal_rmdir(const char *path)
{
    // fdelete_dir requires NO trailing slash
    int len = strlen(path);
    if (len == 0) return -1;

    if (path[len-1] == '/') {
        char *tmp = (char*)malloc(len);
        if (!tmp) return -1;
        memcpy(tmp, path, len-1);
        tmp[len-1] = 0;
        int ret = fdelete_dir(tmp);
        free(tmp);
        return ret;
    }
	return fdelete_dir(path);
}

int aosl_hal_fexist(const char *path)
{
    if (fdir_exist(path)) return 1;

    FILE *fp = fopen(path, "r");
    if (fp) {
        fclose(fp);
        return 1;
    }
	return 0;
}

int aosl_hal_fsize (const char *path)
{
    FILE *fp = fopen(path, "r");
    if (!fp) return -1;
    u32 len = flen(fp);
    fclose(fp);
	return (int)len;
}

int aosl_hal_file_create(const char *filepath)
{
	FILE *fp = fopen(filepath, "w");
	if (!fp) {
		return -1;
	}
	fclose(fp);
	return 0;
}

int aosl_hal_file_delete(const char *filepath)
{
	return fdelete_by_name(filepath);
}

int aosl_hal_file_rename(const char *old_name, const char *new_name)
{
    FILE *fp = fopen(old_name, "r");
    if(!fp) return -1;

    // frename expects only filename, not path
    char *new_base = get_basename(new_name);

    int ret = frename(fp, new_base);
    fclose(fp);
	return ret;
}

aosl_fs_t aosl_hal_fopen(const char *filepath, const char *mode)
{
    char safe_mode[8];
    sanitize_mode(mode, safe_mode);
	return (aosl_fs_t)fopen(filepath, safe_mode);
}

int aosl_hal_fclose(aosl_fs_t fs)
{
    if(!fs) return -1;
	return fclose((FILE *)fs);
}

int aosl_hal_fread (aosl_fs_t fs, void *buf, size_t size)
{
    if(!fs) return -1;
	return fread(buf, 1, size, (FILE *)fs);
}

int aosl_hal_fwrite (aosl_fs_t fs, const void *buf, size_t size)
{
    if(!fs) return -1;
	return fwrite((void*)buf, 1, size, (FILE *)fs);
}
