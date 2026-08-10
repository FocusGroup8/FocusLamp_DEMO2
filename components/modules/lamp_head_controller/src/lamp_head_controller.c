/*
 * lamp_head_controller.c - Remote lamp head controller implementation
 *
 * Sends HTTP POST requests to the camera-fps board's REST API
 * to control head LED and expressive eyes.
 */

#include "lamp_head_controller.h"
#include "lamp_head_controller_config.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "lamp_head";

#define HTTP_TIMEOUT_MS 3000
#define URL_BUF_SIZE   128
#define JSON_BUF_SIZE  128

static esp_err_t send_post_request(const char *path, const char *json_body)
{
    const char *target_ip = LAMP_HEAD_TARGET_IP;
    if (target_ip[0] == '\0' || strcmp(target_ip, "0.0.0.0") == 0) {
        ESP_LOGW(TAG, "Lamp head target IP not configured");
        return ESP_ERR_INVALID_STATE;
    }

    char url[URL_BUF_SIZE];
    snprintf(url, sizeof(url), "http://%s%s", target_ip, path);

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = HTTP_TIMEOUT_MS,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "HTTP client init failed");
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    if (json_body && json_body[0] != '\0') {
        esp_http_client_set_post_field(client, json_body, strlen(json_body));
    }

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        int status_code = esp_http_client_get_status_code(client);
        if (status_code != 200) {
            ESP_LOGW(TAG, "POST %s -> HTTP %d", path, status_code);
            err = ESP_ERR_INVALID_RESPONSE;
        }
    } else {
        ESP_LOGW(TAG, "POST %s failed: %s", path, esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
    return err;
}

/* Default head light brightness (%). The ambient light sensor module has
 * been removed from the hardware, so brightness is a fixed value. */
#define LAMP_HEAD_DEFAULT_BRIGHTNESS 60

esp_err_t lamp_head_led_on(uint8_t brightness)
{
    if (brightness > 100) {
        brightness = 100;
    }

    char json[JSON_BUF_SIZE];
    snprintf(json, sizeof(json), "{\"brightness\":%d,\"color_temp\":50}", brightness);

    ESP_LOGI(TAG, "Head LED on, brightness=%d%%", brightness);
    return send_post_request("/api/led/on", json);
}

esp_err_t lamp_head_led_on_with_ambient(void)
{
    /* Fixed brightness (light sensor removed) */
    return lamp_head_led_on(LAMP_HEAD_DEFAULT_BRIGHTNESS);
}

esp_err_t lamp_head_led_off(void)
{
    ESP_LOGI(TAG, "Head LED off");
    return send_post_request("/api/led/off", "{}");
}

esp_err_t lamp_head_set_expression(const char *expression)
{
    if (!expression || expression[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    char json[JSON_BUF_SIZE];
    snprintf(json, sizeof(json), "{\"expression\":\"%s\"}", expression);

    ESP_LOGI(TAG, "Head expression: %s", expression);
    return send_post_request("/api/eyes/expression", json);
}
