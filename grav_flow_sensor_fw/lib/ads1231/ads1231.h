#pragma once

#include <cstdint>
#include "driver/gpio.h"
#include "esp_err.h"

class ADS1231 {
public:
    struct Sample {
        int32_t raw;        // signed 24-bit ADC value sign-extended to int32_t
        float normalized;   // approximately -1.0 to +1.0
    };

    ADS1231(gpio_num_t sclk_gpio, gpio_num_t dout_gpio);

    esp_err_t begin();

    bool isReady() const;

    // Non-blocking read.
    // Returns:
    //   ESP_OK                sample was read
    //   ESP_ERR_INVALID_STATE data not ready
    //   other esp_err_t       GPIO error
    esp_err_t readRaw(int32_t &value);

    // Convenient function for a 100 ms periodic task.
    // Returns true only when a new sample was read.
    bool poll(Sample &sample);

private:
    gpio_num_t _sclk;
    gpio_num_t _dout;

    void delayShort() const;
    void clockPulse() const;
};