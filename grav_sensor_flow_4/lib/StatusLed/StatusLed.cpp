#include "StatusLed.h"

#include "esp_log.h"

namespace {

const char *TAG = "LED";

constexpr uint32_t RMT_RESOLUTION_HZ = 10000000;                        // 0.1 us per tick
constexpr uint16_t WS2812_T0H = 3;                                      // bit 0: 0.3 us high, 0.9 us low
constexpr uint16_t WS2812_T0L = 9;
constexpr uint16_t WS2812_T1H = 9;                                      // bit 1: 0.9 us high, 0.3 us low
constexpr uint16_t WS2812_T1L = 3;
constexpr size_t RMT_MEM_BLOCK_SYMBOLS = 48;                            // one LED is 24 symbols
constexpr uint32_t BLINK_HALF_PERIOD_MS = 500;
constexpr uint32_t WRITE_TIMEOUT_MS = 10;                               // 24 bits take 30 us

}  // namespace

StatusLed::StatusLed(gpio_num_t gpio, uint8_t green_level) : _gpio(gpio), _green_level(green_level) {}

esp_err_t StatusLed::begin() {
    rmt_tx_channel_config_t channel_config = {};
    channel_config.gpio_num = _gpio;
    channel_config.clk_src = RMT_CLK_SRC_DEFAULT;
    channel_config.resolution_hz = RMT_RESOLUTION_HZ;
    channel_config.mem_block_symbols = RMT_MEM_BLOCK_SYMBOLS;
    channel_config.trans_queue_depth = 1;
    esp_err_t err = rmt_new_tx_channel(&channel_config, &_channel);
    if (err != ESP_OK)
        return err;

    rmt_bytes_encoder_config_t encoder_config = {};
    encoder_config.bit0.level0 = 1;
    encoder_config.bit0.duration0 = WS2812_T0H;
    encoder_config.bit0.level1 = 0;
    encoder_config.bit0.duration1 = WS2812_T0L;
    encoder_config.bit1.level0 = 1;
    encoder_config.bit1.duration0 = WS2812_T1H;
    encoder_config.bit1.level1 = 0;
    encoder_config.bit1.duration1 = WS2812_T1L;
    encoder_config.flags.msb_first = 1;
    err = rmt_new_bytes_encoder(&encoder_config, &_encoder);
    if (err != ESP_OK)
        return err;

    err = rmt_enable(_channel);
    if (err != ESP_OK)
        return err;
    err = write_green(0);                                               // LED may hold a color from before reset
    if (err != ESP_OK)
        return err;

    if (xTaskCreate(task_entry, "status_led", 2048, this, 3, &_task_handle) != pdPASS)
        return ESP_ERR_NO_MEM;
    return ESP_OK;
}

void StatusLed::set(Mode mode) {
    _mode = mode;
    if (_task_handle != nullptr)
        xTaskNotifyGive(_task_handle);
}

esp_err_t StatusLed::write_green(uint8_t level) {
    const uint8_t grb[3] = {level, 0, 0};                               // WS2812 byte order is green, red, blue
    rmt_transmit_config_t transmit_config = {};                         // line idles low, > 50 us low latches the color
    esp_err_t err = rmt_transmit(_channel, _encoder, grb, sizeof(grb), &transmit_config);
    if (err != ESP_OK)
        return err;
    return rmt_tx_wait_all_done(_channel, WRITE_TIMEOUT_MS);
}

void StatusLed::task_entry(void *arg) {
    static_cast<StatusLed *>(arg)->task();
}

void StatusLed::task() {
    bool lit = false;
    TickType_t wait = portMAX_DELAY;
    while (true) {
        const bool mode_changed = ulTaskNotifyTake(pdTRUE, wait) != 0;
        const Mode mode = _mode;
        if (mode == Mode::blink) {
            lit = mode_changed ? false : !lit;
            wait = pdMS_TO_TICKS(BLINK_HALF_PERIOD_MS);
        } else {
            lit = mode == Mode::on;
            wait = portMAX_DELAY;
        }
        const esp_err_t err = write_green(lit ? _green_level : 0);
        if (err != ESP_OK)
            ESP_LOGE(TAG, "LED write failed: %s", esp_err_to_name(err));
    }
}
