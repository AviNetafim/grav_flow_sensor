#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define ADS1231_DOUT_GPIO          GPIO_NUM_16
#define ADS1231_SCLK_GPIO          GPIO_NUM_17
#define ADS1231_READY_TIMEOUT_MS   1000
#define REPORT_PERIOD_MS           3000
#define TARE_SAMPLES               10
#define MEASUREMENT_SAMPLES        5

/*
 * Replace this after calibration:
 *   counts_per_gram = (raw_with_known_weight - tare_raw) / known_weight_grams
 * A negative value is valid if adding weight makes the raw reading decrease.
 */
#define COUNTS_PER_GRAM            1.0f

static const char *TAG = "load_cell";
static int32_t s_tare_raw;
static portMUX_TYPE s_ads_lock = portMUX_INITIALIZER_UNLOCKED;

static esp_err_t ads1231_init(void)
{
    gpio_config_t dout_config = {};
    dout_config.pin_bit_mask = 1ULL << ADS1231_DOUT_GPIO;
    dout_config.mode = GPIO_MODE_INPUT;
    dout_config.pull_up_en = GPIO_PULLUP_DISABLE;
    dout_config.pull_down_en = GPIO_PULLDOWN_DISABLE;
    dout_config.intr_type = GPIO_INTR_DISABLE;
    ESP_RETURN_ON_ERROR(gpio_config(&dout_config), TAG, "DOUT setup failed");

    gpio_config_t sclk_config = {};
    sclk_config.pin_bit_mask = 1ULL << ADS1231_SCLK_GPIO;
    sclk_config.mode = GPIO_MODE_OUTPUT;
    sclk_config.pull_up_en = GPIO_PULLUP_DISABLE;
    sclk_config.pull_down_en = GPIO_PULLDOWN_DISABLE;
    sclk_config.intr_type = GPIO_INTR_DISABLE;
    ESP_RETURN_ON_ERROR(gpio_config(&sclk_config), TAG, "SCLK setup failed");

    /* SCLK low keeps the converter active. High for >60 us powers it down. */
    return gpio_set_level(ADS1231_SCLK_GPIO, 0);
}

static esp_err_t ads1231_read_raw(int32_t *result, uint32_t timeout_ms)
{
    const TickType_t start = xTaskGetTickCount();
    const TickType_t timeout = pdMS_TO_TICKS(timeout_ms);

    while (gpio_get_level(ADS1231_DOUT_GPIO) != 0) {
        if ((xTaskGetTickCount() - start) >= timeout) {
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    uint32_t value = 0;

    /*
     * Do not allow a context switch while SCLK is high: an SCLK high pulse
     * longer than 60 us puts the ADS1231 into power-down.
     */
    portENTER_CRITICAL(&s_ads_lock);
    for (int bit = 0; bit < 24; ++bit) {
        gpio_set_level(ADS1231_SCLK_GPIO, 1);
        esp_rom_delay_us(1);
        value = (value << 1) | (uint32_t)gpio_get_level(ADS1231_DOUT_GPIO);
        gpio_set_level(ADS1231_SCLK_GPIO, 0);
        esp_rom_delay_us(1);
    }
    portEXIT_CRITICAL(&s_ads_lock);

    /* Sign-extend the ADS1231 24-bit two's-complement result. */
    if ((value & 0x00800000U) != 0) {
        value |= 0xFF000000U;
    }
    *result = (int32_t)value;
    return ESP_OK;
}

static esp_err_t ads1231_average(int samples, int32_t *average)
{
    int64_t sum = 0;
    for (int i = 0; i < samples; ++i) {
        int32_t raw;
        const esp_err_t err =
            ads1231_read_raw(&raw, ADS1231_READY_TIMEOUT_MS);
        if (err != ESP_OK) {
            return err;
        }
        sum += raw;
    }
    *average = (int32_t)(sum / samples);
    return ESP_OK;
}

extern "C" void app_main(void)
{
    ESP_ERROR_CHECK(ads1231_init());
    ESP_LOGI(TAG, "ADS1231: DOUT=GPIO%d, SCLK=GPIO%d",
             ADS1231_DOUT_GPIO, ADS1231_SCLK_GPIO);
    ESP_LOGI(TAG, "Keep the load cell empty while tare is measured...");

    esp_err_t err = ads1231_average(TARE_SAMPLES, &s_tare_raw);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ADS1231 not ready during tare: %s",
                 esp_err_to_name(err));
        ESP_LOGE(TAG, "Check power, wiring, and that DOUT becomes low.");
    } else {
        ESP_LOGI(TAG, "Tare complete: raw=%" PRId32, s_tare_raw);
    }

    TickType_t next_report = xTaskGetTickCount();
    while (true) {
        int32_t raw;
        err = ads1231_average(MEASUREMENT_SAMPLES, &raw);

        if (err == ESP_OK) {
            const float weight_g =
                (float)(raw - s_tare_raw) / COUNTS_PER_GRAM;
            ESP_LOGI(TAG, "raw=%" PRId32 ", weight=%.2f g", raw, weight_g);
        } else {
            ESP_LOGE(TAG, "Read failed: %s", esp_err_to_name(err));
        }

        vTaskDelayUntil(&next_report, pdMS_TO_TICKS(REPORT_PERIOD_MS));
    }
}
