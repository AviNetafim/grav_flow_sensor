#include "ads1231.h"

#include "esp_rom_sys.h"

ADS1231::ADS1231(gpio_num_t sclk_gpio, gpio_num_t dout_gpio)
    : _sclk(sclk_gpio), _dout(dout_gpio)
{
}

esp_err_t ADS1231::begin()
{
    gpio_config_t sclk_conf = {};
    sclk_conf.pin_bit_mask = 1ULL << _sclk;
    sclk_conf.mode = GPIO_MODE_OUTPUT;
    sclk_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    sclk_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    sclk_conf.intr_type = GPIO_INTR_DISABLE;

    esp_err_t err = gpio_config(&sclk_conf);
    if (err != ESP_OK) {
        return err;
    }

    gpio_config_t dout_conf = {};
    dout_conf.pin_bit_mask = 1ULL << _dout;
    dout_conf.mode = GPIO_MODE_INPUT;
    dout_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    dout_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    dout_conf.intr_type = GPIO_INTR_DISABLE;

    err = gpio_config(&dout_conf);
    if (err != ESP_OK) {
        return err;
    }

    // ADS1231 serial clock should idle LOW.
    gpio_set_level(_sclk, 0);

    return ESP_OK;
}

bool ADS1231::isReady() const
{
    // DRDY/DOUT goes LOW when a new conversion result is ready.
    return gpio_get_level(_dout) == 0;
}

void ADS1231::delayShort() const
{
    // ADS1231 serial interface is slow; this is intentionally conservative.
    // 2 us high / low gives about 250 kHz bit clock.
    esp_rom_delay_us(2);
}

void ADS1231::clockPulse() const
{
    gpio_set_level(_sclk, 1);
    delayShort();

    gpio_set_level(_sclk, 0);
    delayShort();
}

esp_err_t ADS1231::readRaw(int32_t &value)
{
    if (!isReady()) {
        return ESP_ERR_INVALID_STATE;
    }

    uint32_t raw = 0;

    // Read 24 bits, MSB first.
    for (int i = 0; i < 24; ++i) {
        gpio_set_level(_sclk, 1);
        delayShort();

        raw <<= 1;
        if (gpio_get_level(_dout)) {
            raw |= 1;
        }

        gpio_set_level(_sclk, 0);
        delayShort();
    }

    // 25th clock pulse completes the read cycle and returns DRDY/DOUT high.
    clockPulse();

    // Sign-extend 24-bit two's-complement value to int32_t.
    if (raw & 0x800000) {
        raw |= 0xFF000000;
    }

    value = static_cast<int32_t>(raw);

    return ESP_OK;
}

bool ADS1231::poll(Sample &sample)
{
    int32_t raw_value = 0;

    esp_err_t err = readRaw(raw_value);
    if (err != ESP_OK) {
        return false;
    }

    sample.raw = raw_value;

    // Signed 24-bit full scale is approximately:
    // -8388608 to +8388607
    sample.normalized = static_cast<float>(raw_value) / 8388608.0f;

    return true;
}