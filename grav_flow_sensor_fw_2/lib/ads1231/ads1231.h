#pragma once

#include <cstdint>
#include "driver/gpio.h"
#include "esp_err.h"

class ADS1231 {
public:
    struct Sample {
        int32_t raw;        // signed 24-bit ADC value sign-extended to int32_t
    };

    ADS1231(gpio_num_t sclk_gpio, gpio_num_t dout_gpio,
            gpio_num_t pwdn_gpio = GPIO_NUM_18);

    esp_err_t begin();
    esp_err_t powerDown();
    esp_err_t powerUp();
    bool isPoweredDown() const;

    esp_err_t isReady(bool &ready) const;

    // Non-blocking read.
    // Returns:
    //   ESP_OK                sample was read
    //   ESP_ERR_NOT_FINISHED  data not ready
    //   ESP_ERR_INVALID_STATE device is not initialized or is powered down
    //   other esp_err_t       GPIO error
    esp_err_t readRaw(int32_t &value);

    // Convenient function for a 100 ms periodic task.
    // Returns ESP_ERR_NOT_FINISHED when no new sample is ready.
    esp_err_t poll(Sample &sample);

private:
    gpio_num_t _sclk;
    gpio_num_t _dout;
    gpio_num_t _pwdn;
    bool _initialized;
    bool _powered_down;

    void delayShort() const;
    void clockPulse() const;
};
