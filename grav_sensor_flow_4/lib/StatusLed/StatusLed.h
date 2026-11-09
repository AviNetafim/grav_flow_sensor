/* Single WS2812 RGB LED driven by RMT, e.g. the esp32-c6-devkitm-1 LED on GPIO8.
 * Shows one green level: off, on, or blinking 0.5 s on / 0.5 s off.
 * A task of its own does the blinking, set() returns at once and can be called from any task.
 */

#ifndef STATUS_LED_h
#define STATUS_LED_h

#include <atomic>
#include <cstdint>
#include "driver/gpio.h"
#include "driver/rmt_tx.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

class StatusLed {
public:
    enum class Mode : uint8_t {
        off,
        on,
        blink,                                                          // starts with the off half
    };

    StatusLed(gpio_num_t gpio, uint8_t green_level);
    esp_err_t begin();                                                  // LED off until the first set()
    void set(Mode mode);

private:
    static void task_entry(void *arg);
    void task();
    esp_err_t write_green(uint8_t level);
    gpio_num_t _gpio;
    uint8_t _green_level;                                               // 0..255, 128 = half intensity
    std::atomic<Mode> _mode{Mode::off};
    TaskHandle_t _task_handle = nullptr;
    rmt_channel_handle_t _channel = nullptr;
    rmt_encoder_handle_t _encoder = nullptr;
};

#endif
