#ifndef AUDIO_STORAGE_H
#define AUDIO_STORAGE_H

#include "esp_err.h"

#include "audio_storage_config.h"

#if (AUDIO_STORAGE_ENABLE == 1)

esp_err_t audio_storage_init(void);
esp_err_t audio_storage_deinit(void);

esp_err_t audio_storage_read_file(const char* path, uint8_t* data, size_t* size);
esp_err_t audio_storage_write_file(const char* path, const uint8_t* data, size_t size);
esp_err_t audio_storage_delete_file(const char* path);
esp_err_t audio_storage_mkdir(const char* path);

esp_err_t audio_storage_list_files(const char* dir_path, char files[][256], int* count);

esp_err_t audio_storage_get_info(size_t* total, size_t* used);

typedef void* audio_storage_file_t;

audio_storage_file_t audio_storage_fopen(const char* path, const char* mode);
size_t audio_storage_fread(void* ptr, size_t size, size_t nmemb, audio_storage_file_t stream);
int    audio_storage_fseek(audio_storage_file_t stream, long offset, int whence);
long   audio_storage_ftell(audio_storage_file_t stream);
int    audio_storage_fclose(audio_storage_file_t stream);

#else

static inline esp_err_t audio_storage_init(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}
static inline esp_err_t audio_storage_deinit(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

static inline esp_err_t audio_storage_read_file(const char* path, uint8_t* data, size_t* size)
{
    (void)path;
    (void)data;
    (void)size;
    return ESP_ERR_NOT_SUPPORTED;
}

static inline esp_err_t audio_storage_write_file(const char* path, const uint8_t* data, size_t size)
{
    (void)path;
    (void)data;
    (void)size;
    return ESP_ERR_NOT_SUPPORTED;
}

static inline esp_err_t audio_storage_delete_file(const char* path)
{
    (void)path;
    return ESP_ERR_NOT_SUPPORTED;
}

static inline esp_err_t audio_storage_mkdir(const char* path)
{
    (void)path;
    return ESP_ERR_NOT_SUPPORTED;
}

static inline esp_err_t audio_storage_list_files(const char* dir_path, char files[][256],
                                                 int* count)
{
    (void)dir_path;
    (void)files;
    (void)count;
    return ESP_ERR_NOT_SUPPORTED;
}

static inline esp_err_t audio_storage_get_info(size_t* total, size_t* used)
{
    (void)total;
    (void)used;
    return ESP_ERR_NOT_SUPPORTED;
}

typedef void* audio_storage_file_t;

static inline audio_storage_file_t audio_storage_fopen(const char* path, const char* mode)
{
    (void)path;
    (void)mode;
    return NULL;
}

static inline size_t audio_storage_fread(void* ptr, size_t size, size_t nmemb,
                                         audio_storage_file_t stream)
{
    (void)ptr;
    (void)size;
    (void)nmemb;
    (void)stream;
    return 0;
}

static inline int audio_storage_fseek(audio_storage_file_t stream, long offset, int whence)
{
    (void)stream;
    (void)offset;
    (void)whence;
    return -1;
}

static inline long audio_storage_ftell(audio_storage_file_t stream)
{
    (void)stream;
    return -1;
}

static inline int audio_storage_fclose(audio_storage_file_t stream)
{
    (void)stream;
    return -1;
}

#endif

#endif
