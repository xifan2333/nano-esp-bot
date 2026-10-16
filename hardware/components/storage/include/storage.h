#ifndef STORAGE_H
#define STORAGE_H

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define STORAGE_BASE_PATH "/storage"
#define STORAGE_PARTITION_LABEL "storage"
#define STORAGE_MAX_OPEN_FILES 8
#define STORAGE_PATH_MAX_LEN 300

/**
 * @brief Initialize and mount the SPIFFS filesystem at /storage.
 *
 * Automatically formats the partition on first boot if unformatted.
 *
 * @return esp_err_t ESP_OK on success.
 */
esp_err_t storage_init(void);

/**
 * @brief Unmount the SPIFFS filesystem.
 *
 * @return esp_err_t ESP_OK on success.
 */
esp_err_t storage_deinit(void);

/**
 * @brief Check if the SPIFFS filesystem is currently mounted.
 *
 * @return true if mounted, false otherwise.
 */
bool storage_is_mounted(void);

/**
 * @brief Get filesystem capacity and usage metrics in bytes.
 *
 * @param out_total Total usable space in bytes.
 * @param out_used Currently used space in bytes.
 * @return esp_err_t ESP_OK on success.
 */
esp_err_t storage_get_info(size_t *out_total, size_t *out_used);

/**
 * @brief Check if a relative file path exists in storage.
 *
 * @param rel_path Path within storage (e.g. "init.lua").
 * @return true if file exists, false otherwise.
 */
bool storage_file_exists(const char *rel_path);

/**
 * @brief Read file content into a memory buffer.
 *
 * @param rel_path Path within storage.
 * @param out_buf Buffer to store read content.
 * @param buf_size Size of out_buf.
 * @param out_len Number of bytes read.
 * @return esp_err_t ESP_OK on success.
 */
esp_err_t storage_read_file(const char *rel_path, char *out_buf, size_t buf_size, size_t *out_len);

/**
 * @brief Write data to a file in storage.
 *
 * @param rel_path Path within storage.
 * @param data Data buffer to write.
 * @param data_len Number of bytes to write.
 * @return esp_err_t ESP_OK on success.
 */
esp_err_t storage_write_file(const char *rel_path, const char *data, size_t data_len);

/**
 * @brief Delete a file from storage.
 *
 * @param rel_path Path within storage.
 * @return esp_err_t ESP_OK on success.
 */
esp_err_t storage_delete_file(const char *rel_path);

/**
 * @brief Callback invoked for each regular file discovered during directory iteration.
 *
 * @param name File name (e.g. "init.lua").
 * @param size File size in bytes.
 * @param user_data Caller-provided context pointer.
 */
typedef void (*storage_list_cb_t)(const char *name, size_t size, void *user_data);

/**
 * @brief Enumerate all files stored in the SPIFFS filesystem.
 *
 * @param cb Callback called for every file found.
 * @param user_data User pointer passed to cb.
 * @return esp_err_t ESP_OK on success.
 */
esp_err_t storage_list_files(storage_list_cb_t cb, void *user_data);

#ifdef __cplusplus
}
#endif

#endif /* STORAGE_H */
