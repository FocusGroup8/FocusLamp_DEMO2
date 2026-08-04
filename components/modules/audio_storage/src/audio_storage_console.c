#include "esp_console.h"
#include "esp_log.h"

#include "argtable3/argtable3.h"
#include "audio_storage.h"

#if (AUDIO_STORAGE_ENABLE == 1)

static const char* TAG = "audio_store_console";

static struct
{
    struct arg_end* end;
} audio_store_list_args;

static struct
{
    struct arg_end* end;
} audio_store_info_args;

static struct
{
    struct arg_str* filename;
    struct arg_int* size;
    struct arg_end* end;
} audio_store_write_args;

static struct
{
    struct arg_str* filename;
    struct arg_end* end;
} audio_store_read_args;

static struct
{
    struct arg_str* filename;
    struct arg_end* end;
} audio_store_delete_args;

static int cmd_audio_store_list(int argc, char** argv)
{
    int nerrors = arg_parse(argc, argv, (void**)&audio_store_list_args);
    if (nerrors != 0)
    {
        arg_print_errors(stderr, audio_store_list_args.end, argv[0]);
        return 1;
    }

    printf("\n========================================\n");
    printf("Audio Storage - File List\n");
    printf("========================================\n\n");

    char (*files)[256] = malloc(32 * 256);
    if (files == NULL)
    {
        printf("Failed to allocate memory\n");
        return 1;
    }

    int file_count = 32;

    esp_err_t ret = audio_storage_list_files("/", files, &file_count);
    if (ret != ESP_OK)
    {
        printf("Failed to list files: %s\n", esp_err_to_name(ret));
        free(files);
        return 1;
    }

    if (file_count == 0)
    {
        printf("No files found in storage partition\n");
    }
    else
    {
        printf("Total files: %d\n\n", file_count);
        for (int i = 0; i < file_count; i++)
        {
            printf("%2d. %s\n", i + 1, files[i]);
        }
    }

    free(files);
    printf("\n========================================\n\n");
    return 0;
}

static int cmd_audio_store_info(int argc, char** argv)
{
    int nerrors = arg_parse(argc, argv, (void**)&audio_store_info_args);
    if (nerrors != 0)
    {
        arg_print_errors(stderr, audio_store_info_args.end, argv[0]);
        return 1;
    }

    printf("\n========================================\n");
    printf("Audio Storage - Partition Info\n");
    printf("========================================\n\n");

    size_t total_size = 0;
    size_t used_size  = 0;

    esp_err_t ret = audio_storage_get_info(&total_size, &used_size);
    if (ret != ESP_OK)
    {
        printf("Failed to get partition info: %s\n", esp_err_to_name(ret));
        return 1;
    }

    size_t free_size = total_size - used_size;

    printf("Total size:  %zu bytes (%.2f KB)\n", total_size, total_size / 1024.0);
    printf("Used size:   %zu bytes (%.2f KB)\n", used_size, used_size / 1024.0);
    printf("Free size:   %zu bytes (%.2f KB)\n", free_size, free_size / 1024.0);
    printf("Usage:       %.1f%%\n", (float)used_size / total_size * 100);

    printf("\n========================================\n\n");
    return 0;
}

static int cmd_audio_store_write(int argc, char** argv)
{
    int nerrors = arg_parse(argc, argv, (void**)&audio_store_write_args);
    if (nerrors != 0)
    {
        arg_print_errors(stderr, audio_store_write_args.end, argv[0]);
        return 1;
    }

    const char* filename = audio_store_write_args.filename->sval[0];
    int         size     = audio_store_write_args.size->ival[0];

    if (size <= 0)
    {
        printf("Error: Invalid size\n");
        return 1;
    }

    printf("\n========================================\n");
    printf("Audio Storage - Write Test File\n");
    printf("========================================\n\n");

    printf("Filename: %s\n", filename);
    printf("Size: %d bytes\n\n", size);

    uint8_t* data = malloc(size);
    if (data == NULL)
    {
        printf("Failed to allocate memory\n");
        return 1;
    }

    for (int i = 0; i < size; i++)
    {
        data[i] = (uint8_t)(i & 0xFF);
    }

    esp_err_t ret = audio_storage_write_file(filename, data, size);
    free(data);

    if (ret != ESP_OK)
    {
        printf("Failed to write file: %s\n", esp_err_to_name(ret));
        return 1;
    }

    printf("File written successfully\n");
    printf("\n========================================\n\n");
    return 0;
}

static int cmd_audio_store_read(int argc, char** argv)
{
    int nerrors = arg_parse(argc, argv, (void**)&audio_store_read_args);
    if (nerrors != 0)
    {
        arg_print_errors(stderr, audio_store_read_args.end, argv[0]);
        return 1;
    }

    const char* filename = audio_store_read_args.filename->sval[0];

    printf("\n========================================\n");
    printf("Audio Storage - Read File\n");
    printf("========================================\n\n");

    audio_storage_file_t file = audio_storage_fopen(filename, "rb");
    if (file == NULL)
    {
        printf("Failed to open file\n");
        return 1;
    }

    audio_storage_fseek(file, 0, 2);
    long size = audio_storage_ftell(file);
    audio_storage_fseek(file, 0, 0);

    printf("Filename: %s\n", filename);
    printf("Size: %ld bytes (%.2f KB)\n\n", size, size / 1024.0);

    uint8_t* data = malloc(size);
    if (data == NULL)
    {
        printf("Failed to allocate memory\n");
        audio_storage_fclose(file);
        return 1;
    }

    size_t read_size = audio_storage_fread(data, 1, size, file);
    audio_storage_fclose(file);

    if (read_size != (size_t)size)
    {
        printf("Failed to read file\n");
        free(data);
        return 1;
    }

    printf("First 16 bytes:\n");
    for (int i = 0; i < 16 && i < size; i++)
    {
        printf("%02X ", data[i]);
    }
    printf("\n");

    free(data);
    printf("\n========================================\n\n");
    return 0;
}

static int cmd_audio_store_delete(int argc, char** argv)
{
    int nerrors = arg_parse(argc, argv, (void**)&audio_store_delete_args);
    if (nerrors != 0)
    {
        arg_print_errors(stderr, audio_store_delete_args.end, argv[0]);
        return 1;
    }

    const char* filename = audio_store_delete_args.filename->sval[0];

    printf("\n========================================\n");
    printf("Audio Storage - Delete File\n");
    printf("========================================\n\n");

    printf("Filename: %s\n\n", filename);

    esp_err_t ret = audio_storage_delete_file(filename);
    if (ret != ESP_OK)
    {
        printf("Failed to delete file: %s\n", esp_err_to_name(ret));
        return 1;
    }

    printf("File deleted successfully\n");
    printf("\n========================================\n\n");
    return 0;
}

esp_err_t audio_storage_register_console_commands(void)
{
    audio_store_list_args.end        = arg_end(1);
    const esp_console_cmd_t list_cmd = {
        .command  = "audio_store_list",
        .help     = "List all files in audio storage",
        .func     = &cmd_audio_store_list,
        .argtable = &audio_store_list_args,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&list_cmd));

    audio_store_info_args.end        = arg_end(1);
    const esp_console_cmd_t info_cmd = {
        .command  = "audio_store_info",
        .help     = "Show audio storage partition info",
        .func     = &cmd_audio_store_info,
        .argtable = &audio_store_info_args,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&info_cmd));

    audio_store_write_args.filename   = arg_str1(NULL, NULL, "<filename>", "Filename to write");
    audio_store_write_args.size       = arg_int1(NULL, NULL, "<size>", "File size in bytes");
    audio_store_write_args.end        = arg_end(2);
    const esp_console_cmd_t write_cmd = {
        .command  = "audio_store_write",
        .help     = "Write a test file to audio storage",
        .func     = &cmd_audio_store_write,
        .argtable = &audio_store_write_args,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&write_cmd));

    audio_store_read_args.filename   = arg_str1(NULL, NULL, "<filename>", "Filename to read");
    audio_store_read_args.end        = arg_end(1);
    const esp_console_cmd_t read_cmd = {
        .command  = "audio_store_read",
        .help     = "Read a file from audio storage",
        .func     = &cmd_audio_store_read,
        .argtable = &audio_store_read_args,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&read_cmd));

    audio_store_delete_args.filename   = arg_str1(NULL, NULL, "<filename>", "Filename to delete");
    audio_store_delete_args.end        = arg_end(1);
    const esp_console_cmd_t delete_cmd = {
        .command  = "audio_store_delete",
        .help     = "Delete a file from audio storage",
        .func     = &cmd_audio_store_delete,
        .argtable = &audio_store_delete_args,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&delete_cmd));

    ESP_LOGI(TAG, "Audio storage console commands registered");
    return ESP_OK;
}

#else

esp_err_t audio_storage_register_console_commands(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

#endif
