#include "audio_storage.h"

#include "audio_storage_config.h"

#if (AUDIO_STORAGE_ENABLE == 1)

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <sys/stat.h>

#include "esp_littlefs.h"
#include "esp_log.h"

static const char* TAG = "audio_storage";

static bool s_audio_storage_initialized = false;

esp_err_t audio_storage_init(void)
{
    if (s_audio_storage_initialized)
    {
        ESP_LOGW(TAG, "Audio storage already initialized");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing audio storage...");

    esp_vfs_littlefs_conf_t conf = {
        .base_path              = AUDIO_STORAGE_BASE_PATH,
        .partition_label        = AUDIO_STORAGE_PARTITION_LABEL,
        .format_if_mount_failed = AUDIO_STORAGE_FORMAT_IF_MOUNT_FAILED,
        .dont_mount             = false,
    };

    esp_err_t ret = esp_vfs_littlefs_register(&conf);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to mount LittleFS partition: %s", esp_err_to_name(ret));

        if (ret == ESP_ERR_NO_MEM)
        {
            ESP_LOGI(TAG, "Attempting to format LittleFS partition...");
            ret = esp_littlefs_format(AUDIO_STORAGE_PARTITION_LABEL);
            if (ret != ESP_OK)
            {
                ESP_LOGE(TAG, "Failed to format LittleFS partition: %s", esp_err_to_name(ret));
                return ret;
            }

            ESP_LOGI(TAG, "LittleFS partition formatted, attempting to mount again...");
            ret = esp_vfs_littlefs_register(&conf);
            if (ret != ESP_OK)
            {
                ESP_LOGE(TAG, "Failed to mount LittleFS partition after format: %s",
                         esp_err_to_name(ret));
                return ret;
            }
        }
        else
        {
            return ret;
        }
    }

    s_audio_storage_initialized = true;

    size_t total = 0, used = 0;
    ret = audio_storage_get_info(&total, &used);
    if (ret == ESP_OK)
    {
        ESP_LOGI(TAG, "Audio storage initialized successfully");
        ESP_LOGI(TAG, "Partition size: total: %.2f KB, used: %.2f KB", total / 1024.0,
                 used / 1024.0);
    }

    return ESP_OK;
}

esp_err_t audio_storage_deinit(void)
{
    if (!s_audio_storage_initialized)
    {
        ESP_LOGW(TAG, "Audio storage not initialized");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Deinitializing audio storage...");

    esp_err_t ret = esp_vfs_littlefs_unregister(AUDIO_STORAGE_PARTITION_LABEL);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to unmount LittleFS: %s", esp_err_to_name(ret));
        return ret;
    }

    s_audio_storage_initialized = false;
    ESP_LOGI(TAG, "Audio storage deinitialized successfully");
    return ESP_OK;
}

esp_err_t audio_storage_read_file(const char* path, uint8_t* data, size_t* size)
{
    if (!s_audio_storage_initialized)
    {
        ESP_LOGE(TAG, "Audio storage not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (path == NULL || data == NULL || size == NULL)
    {
        ESP_LOGE(TAG, "Invalid parameters");
        return ESP_ERR_INVALID_ARG;
    }

    char full_path[512];
    if (path[0] == '/')
    {
        snprintf(full_path, sizeof(full_path), "%s%s", AUDIO_STORAGE_BASE_PATH, path);
    }
    else
    {
        snprintf(full_path, sizeof(full_path), "%s/%s", AUDIO_STORAGE_BASE_PATH, path);
    }

    FILE* f = fopen(full_path, "rb");
    if (f == NULL)
    {
        ESP_LOGE(TAG, "Failed to open file for reading: %s", full_path);
        return ESP_ERR_NOT_FOUND;
    }

    size_t bytes_read = fread(data, 1, *size, f);
    fclose(f);

    *size = bytes_read;
    ESP_LOGD(TAG, "Read %d bytes from file: %s", bytes_read, full_path);
    return ESP_OK;
}

esp_err_t audio_storage_write_file(const char* path, const uint8_t* data, size_t size)
{
    if (!s_audio_storage_initialized)
    {
        ESP_LOGE(TAG, "Audio storage not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (path == NULL || data == NULL)
    {
        ESP_LOGE(TAG, "Invalid parameters");
        return ESP_ERR_INVALID_ARG;
    }

    char full_path[512];
    if (path[0] == '/')
    {
        snprintf(full_path, sizeof(full_path), "%s%s", AUDIO_STORAGE_BASE_PATH, path);
    }
    else
    {
        snprintf(full_path, sizeof(full_path), "%s/%s", AUDIO_STORAGE_BASE_PATH, path);
    }

    FILE* f = fopen(full_path, "wb");
    if (f == NULL)
    {
        ESP_LOGE(TAG, "Failed to open file for writing: %s", full_path);
        return ESP_ERR_NOT_FOUND;
    }

    size_t bytes_written = fwrite(data, 1, size, f);
    fclose(f);

    if (bytes_written != size)
    {
        ESP_LOGE(TAG, "Failed to write all data to file: %s", full_path);
        return ESP_FAIL;
    }

    ESP_LOGD(TAG, "Wrote %d bytes to file: %s", bytes_written, full_path);
    return ESP_OK;
}

esp_err_t audio_storage_delete_file(const char* path)
{
    if (!s_audio_storage_initialized)
    {
        ESP_LOGE(TAG, "Audio storage not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (path == NULL)
    {
        ESP_LOGE(TAG, "Invalid parameters");
        return ESP_ERR_INVALID_ARG;
    }

    char full_path[512];
    if (path[0] == '/')
    {
        snprintf(full_path, sizeof(full_path), "%s%s", AUDIO_STORAGE_BASE_PATH, path);
    }
    else
    {
        snprintf(full_path, sizeof(full_path), "%s/%s", AUDIO_STORAGE_BASE_PATH, path);
    }

    if (remove(full_path) != 0)
    {
        ESP_LOGE(TAG, "Failed to delete file: %s", full_path);
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGD(TAG, "Deleted file: %s", full_path);
    return ESP_OK;
}

esp_err_t audio_storage_mkdir(const char* path)
{
    if (!s_audio_storage_initialized)
    {
        ESP_LOGE(TAG, "Audio storage not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (path == NULL)
    {
        ESP_LOGE(TAG, "Invalid parameters");
        return ESP_ERR_INVALID_ARG;
    }

    char full_path[512];
    if (path[0] == '/')
    {
        snprintf(full_path, sizeof(full_path), "%s%s", AUDIO_STORAGE_BASE_PATH, path);
    }
    else
    {
        snprintf(full_path, sizeof(full_path), "%s/%s", AUDIO_STORAGE_BASE_PATH, path);
    }

    if (mkdir(full_path, 0755) != 0)
    {
        ESP_LOGE(TAG, "Failed to create directory: %s", full_path);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Created directory: %s", full_path);
    return ESP_OK;
}

esp_err_t audio_storage_list_files(const char* dir_path, char files[][256], int* count)
{
    if (!s_audio_storage_initialized)
    {
        ESP_LOGE(TAG, "Audio storage not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (dir_path == NULL || files == NULL || count == NULL)
    {
        ESP_LOGE(TAG, "Invalid parameters");
        return ESP_ERR_INVALID_ARG;
    }

    char full_path[512];
    if (dir_path[0] == '/')
    {
        snprintf(full_path, sizeof(full_path), "%s%s", AUDIO_STORAGE_BASE_PATH, dir_path);
    }
    else
    {
        snprintf(full_path, sizeof(full_path), "%s/%s", AUDIO_STORAGE_BASE_PATH, dir_path);
    }

    DIR* dir = opendir(full_path);
    if (dir == NULL)
    {
        ESP_LOGE(TAG, "Failed to open directory: %s", full_path);
        return ESP_ERR_NOT_FOUND;
    }

    struct dirent* entry;
    int            file_count = 0;
    while ((entry = readdir(dir)) != NULL && file_count < *count)
    {
        strncpy(files[file_count], entry->d_name, 255);
        files[file_count][255] = '\0';
        file_count++;
    }
    closedir(dir);

    *count = file_count;
    ESP_LOGD(TAG, "Listed %d files in directory: %s", file_count, full_path);
    return ESP_OK;
}

esp_err_t audio_storage_get_info(size_t* total, size_t* used)
{
    if (!s_audio_storage_initialized)
    {
        ESP_LOGE(TAG, "Audio storage not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (total == NULL || used == NULL)
    {
        ESP_LOGE(TAG, "Invalid parameters");
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = esp_littlefs_info(AUDIO_STORAGE_PARTITION_LABEL, total, used);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to get partition info: %s", esp_err_to_name(ret));
        return ret;
    }

    return ESP_OK;
}

audio_storage_file_t audio_storage_fopen(const char* path, const char* mode)
{
    if (!s_audio_storage_initialized)
    {
        ESP_LOGE(TAG, "Audio storage not initialized");
        return NULL;
    }

    if (path == NULL || mode == NULL)
    {
        ESP_LOGE(TAG, "Invalid parameters");
        return NULL;
    }

    char full_path[512];
    if (path[0] == '/')
    {
        snprintf(full_path, sizeof(full_path), "%s%s", AUDIO_STORAGE_BASE_PATH, path);
    }
    else
    {
        snprintf(full_path, sizeof(full_path), "%s/%s", AUDIO_STORAGE_BASE_PATH, path);
    }

    FILE* f = fopen(full_path, mode);
    if (f == NULL)
    {
        ESP_LOGE(TAG, "Failed to open file: %s", full_path);
        return NULL;
    }

    ESP_LOGD(TAG, "Opened file: %s", full_path);
    return (audio_storage_file_t)f;
}

size_t audio_storage_fread(void* ptr, size_t size, size_t nmemb, audio_storage_file_t stream)
{
    if (stream == NULL || ptr == NULL)
    {
        ESP_LOGE(TAG, "Invalid parameters");
        return 0;
    }

    return fread(ptr, size, nmemb, (FILE*)stream);
}

int audio_storage_fseek(audio_storage_file_t stream, long offset, int whence)
{
    if (stream == NULL)
    {
        ESP_LOGE(TAG, "Invalid parameters");
        return -1;
    }

    return fseek((FILE*)stream, offset, whence);
}

long audio_storage_ftell(audio_storage_file_t stream)
{
    if (stream == NULL)
    {
        ESP_LOGE(TAG, "Invalid parameters");
        return -1;
    }

    return ftell((FILE*)stream);
}

int audio_storage_fclose(audio_storage_file_t stream)
{
    if (stream == NULL)
    {
        ESP_LOGE(TAG, "Invalid parameters");
        return -1;
    }

    ESP_LOGD(TAG, "Closed file");
    return fclose((FILE*)stream);
}

#endif
