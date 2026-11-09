#include <algorithm>
#include <cinttypes>
#include <cstdint>
#include <cstring>

#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "ads1231.h"
#include "NcsiResponder.h"
#include "NetServer.h"
#include "StatusLed.h"

static const char *TAG = "MAIN";

static constexpr bool ADC_READ_CRITICAL = true;                             // true: read ADC inside portENTER_CRITICAL
static constexpr uint32_t WEIGHT_TEST_CYCLE_MS = 10;                        // ADC poll period, a conversion completes approx every 100 ms
static constexpr size_t WEIGHT_TEST_MAX_SAMPLES = 5000;                     // samples array size
static constexpr size_t CALIBRATE_SAMPLES = 25;
static constexpr uint16_t WEIGHT2TIME_MIN_SAMPLES = 10;
static constexpr int64_t CAL_MUL = 100;
static constexpr size_t SAMPLES_PER_S = 10;                                 // inc_timeout in s to samples
static constexpr size_t FLOW_SENSE_SAMPLES = 10;                            // flow is sensed over this many samples (1 s)
static constexpr int32_t FLOW_SENSE_10MG = 50;                              // weight increment that means flow started (0.5 g)

static constexpr int64_t ADC_NOT_READY_TIMEOUT_US = 2000000;                // abort test if no conversion completes for 2 s
static constexpr uint32_t WIFI_SWITCH_TIMEOUT_MS = 5000;                    // abort test if wifi does not stop within 5 s
static constexpr uint32_t CLIENT_CLOSE_TIMEOUT_MS = 2000;                   // max wait for client to get response and disconnect

static constexpr gpio_num_t VOLUME_LOW_GPIO = GPIO_NUM_20;
static constexpr gpio_num_t VOLUME_HIGH_GPIO = GPIO_NUM_19;
static constexpr uint32_t VOLUME_GPIO_SAMPLE_PERIOD_MS = 25;                // 25 ms sample period for volume test task

static constexpr gpio_num_t STATUS_LED_GPIO = GPIO_NUM_8;                   // esp32-c6-devkitm-1 RGB LED
static constexpr uint8_t STATUS_LED_GREEN = 128;                            // half intensity
static constexpr uint16_t NO_TEST = 0xFFFF;                                 // last_test mode before the first test

enum class TestMode : uint16_t {
    weight2time = 0,
    weight2inc = 1,
    volume = 2,
    calibrate = 3,
};

enum class TestState : uint16_t {                                           // weight_stat[0] and vol_stat[0]
    pending = 0,                                                            // no result yet
    done = 1,
    timeout = 2,
    error = 3,
};

static constexpr uint16_t TEST_START = 1;

struct WeightTestData {
    TestState test_state;
    TestMode mode;
    uint16_t target;
    uint16_t inc_timeout_s;
    int16_t cal_div;
    int16_t cal_offset;
    uint16_t stat_1;                                                        // weight_stat[1]
    uint16_t stat_2;                                                        // weight_stat[2]
    uint16_t time_ds;                                                       // weight_stat[4]
    size_t sample_count;
    int32_t samples[WEIGHT_TEST_MAX_SAMPLES];                               // raw for calibrate, 10 mg units otherwise
};

struct VolumeTestData {
    TestState test_state;
    uint16_t time2fill_ds;
    uint16_t timeout_low_s;
    uint16_t timeout_high_s;
    uint16_t stable_time_ms;
};

struct LastTest {                                                           // last_test register, the test get reports on
    uint16_t mode;
    uint16_t target;
};

static ADS1231 adc(GPIO_NUM_17, GPIO_NUM_16, GPIO_NUM_18);
static StatusLed led(STATUS_LED_GPIO, STATUS_LED_GREEN);
static portMUX_TYPE adc_spinlock = portMUX_INITIALIZER_UNLOCKED;
static SemaphoreHandle_t test_mutex;                                        // protects test_running and the stat values
static TaskHandle_t weight_test_task_handle;
static TaskHandle_t volume_test_task_handle;
static bool test_running = false;                                           // one test at a time, weight or volume
// While test_running the weight task writes samples without the mutex, nothing reads them until it clears.
static WeightTestData weight_test_data = {TestState::pending, TestMode::weight2time, 0, 0, 1, 0, 0, 0, 0, 0, {}};
static VolumeTestData volume_test_data = {TestState::pending, 0, 0, 0, 0};
static LastTest last_test = {NO_TEST, 0};

static uint16_t to_register(int64_t value) {                                // signed int16 in two's complement
    return static_cast<uint16_t>(static_cast<int16_t>(std::clamp<int64_t>(value, INT16_MIN, INT16_MAX)));
}

static esp_err_t poll_adc(ADS1231::Sample &sample) {
    if (!ADC_READ_CRITICAL) {
        return adc.poll(sample);
    }
    portENTER_CRITICAL(&adc_spinlock);                                      // interrupts cannot stretch the serial clock
    const esp_err_t err = adc.poll(sample);
    portEXIT_CRITICAL(&adc_spinlock);
    return err;
}

class AdcReader {
public:
    AdcReader() : _next_wake(xTaskGetTickCount()), _last_ready_us(esp_timer_get_time()) {}

    esp_err_t next(int32_t &raw) {                                          // wait for the next conversion
        while (true) {
            vTaskDelayUntil(&_next_wake, pdMS_TO_TICKS(WEIGHT_TEST_CYCLE_MS));
            ADS1231::Sample sample;
            const esp_err_t err = poll_adc(sample);
            const int64_t now_us = esp_timer_get_time();
            if (err == ESP_ERR_NOT_FINISHED) {
                if (now_us - _last_ready_us >= ADC_NOT_READY_TIMEOUT_US) {
                    return ESP_ERR_TIMEOUT;
                }
                continue;
            }
            if (err != ESP_OK) {
                return err;
            }
            _last_ready_us = now_us;
            raw = sample.raw;
            return ESP_OK;
        }
    }

    int64_t last_ready_us() const {                                         // time of the conversion next() returned
        return _last_ready_us;
    }

private:
    TickType_t _next_wake;
    int64_t _last_ready_us;
};

struct WeightResult {
    TestState state;
    uint16_t stat_1;
    uint16_t stat_2;
    uint16_t time_ds;
};

static uint16_t elapsed_ds(int64_t from_us, int64_t to_us) {
    return static_cast<uint16_t>(std::clamp<int64_t>((to_us - from_us) / 100000, 0, UINT16_MAX));
}

static int32_t to_10mg(int32_t raw, int16_t cal_div, int16_t cal_offset) {
    return static_cast<int32_t>(cal_offset + static_cast<int64_t>(raw) * CAL_MUL / cal_div);
}

// calibrate: average raw weight, split to stat_1 (lower 16 bits) and stat_2 (higher 16 bits).
// weight2time: average weight in 10 mg units to stat_1, loop counter to stat_2.
// time_ds: first to last sample.
static WeightResult run_average(AdcReader &reader, TestMode mode, size_t loops, int16_t cal_div, int16_t cal_offset) {
    int64_t sum = 0;
    int64_t first_us = 0;
    for (size_t i = 0; i < loops; i++) {
        int32_t raw;
        const esp_err_t err = reader.next(raw);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "ADC read failed: %s", esp_err_to_name(err));
            return {TestState::error, 0, 0, 0};
        }
        if (i == 0) {
            first_us = reader.last_ready_us();
        }
        const int32_t sample = mode == TestMode::calibrate ? raw : to_10mg(raw, cal_div, cal_offset);
        weight_test_data.samples[i] = sample;
        weight_test_data.sample_count = i + 1;
        sum += sample;
    }
    const int64_t average = sum / static_cast<int64_t>(loops);
    const uint16_t time_ds = elapsed_ds(first_us, reader.last_ready_us());
    if (mode == TestMode::calibrate) {
        const uint32_t raw_average = static_cast<uint32_t>(static_cast<int32_t>(average));
        return {TestState::done, static_cast<uint16_t>(raw_average & 0xFFFF), static_cast<uint16_t>(raw_average >> 16), time_ds};
    }
    return {TestState::done, to_register(average), static_cast<uint16_t>(loops), time_ds};
}

// Sense flow, then read weight until target increment from the tare at flow start, or timeout.
// Timeout also covers the flow sense stage. stat_1: last weight, stat_2: samples from flow start to stop,
// time_ds: tare sample to last sample, 0 if no flow was sensed.
static WeightResult run_weight2inc(AdcReader &reader, uint16_t target, size_t max_samples, int16_t cal_div, int16_t cal_offset) {
    int32_t *samples = weight_test_data.samples;
    bool sense_flow = false;
    int32_t tare = 0;
    int64_t tare_us = 0;
    uint16_t cnt2inc = 0;
    for (size_t i = 0;; i++) {
        int32_t raw;
        const esp_err_t err = reader.next(raw);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "ADC read failed: %s", esp_err_to_name(err));
            return {TestState::error, 0, 0, 0};
        }
        samples[i] = to_10mg(raw, cal_div, cal_offset);
        weight_test_data.sample_count = i + 1;

        if (!sense_flow) {
            if (i >= FLOW_SENSE_SAMPLES && samples[i] - samples[i - FLOW_SENSE_SAMPLES] > FLOW_SENSE_10MG) {
                tare = samples[i];
                tare_us = reader.last_ready_us();
                cnt2inc = 0;
                sense_flow = true;
                ESP_LOGI(TAG, "Flow sensed: sample %u, tare=%ld", static_cast<unsigned>(i), static_cast<long>(tare));
            }
        } else {
            cnt2inc++;
            if (samples[i] > target + tare) {
                return {TestState::done, to_register(samples[i]), cnt2inc, elapsed_ds(tare_us, reader.last_ready_us())};
            }
        }
        if (i + 1 >= max_samples) {
            const uint16_t time_ds = sense_flow ? elapsed_ds(tare_us, reader.last_ready_us()) : 0;
            return {TestState::timeout, to_register(samples[i]), cnt2inc, time_ds};
        }
    }
}

static void weight_test_task(void *arg) {
    NetServer *ns = static_cast<NetServer *>(arg);

    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);                            // wait for reg_action signal

        if (!ns->wait_client_closed(pdMS_TO_TICKS(CLIENT_CLOSE_TIMEOUT_MS))) {  // client closes after it got the response
            ESP_LOGW(TAG, "Client still connected, disconnecting it");
        }
        ESP_LOGI(TAG, "Stopping wifi");
        const esp_err_t wifi_off_err = ns->wifi_off(pdMS_TO_TICKS(WIFI_SWITCH_TIMEOUT_MS));

        // test parameters are only written by reg_action while no test is running
        const TestMode mode = weight_test_data.mode;
        const uint16_t target = weight_test_data.target;
        const int16_t cal_div = weight_test_data.cal_div;
        const int16_t cal_offset = weight_test_data.cal_offset;
        const size_t inc_max_samples =
            std::min<size_t>(weight_test_data.inc_timeout_s * SAMPLES_PER_S, WEIGHT_TEST_MAX_SAMPLES);

        WeightResult result = {TestState::error, 0, 0, 0};
        if (wifi_off_err != ESP_OK) {
            ESP_LOGE(TAG, "Wifi stop failed: %s", esp_err_to_name(wifi_off_err));
        } else {
            led.set(StatusLed::Mode::off);                                  // wifi off, then blinks while the test runs
            led.set(StatusLed::Mode::blink);
            ESP_LOGI(TAG, "Weight test started: mode=%u, target=%u, cal_div=%d, cal_offset=%d",
                     static_cast<unsigned>(mode), target, cal_div, cal_offset);
            const int64_t started_us = esp_timer_get_time();
            AdcReader reader;
            int32_t stale_raw;                                              // conversion already waiting at start has unknown age
            const esp_err_t err = reader.next(stale_raw);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "ADC read failed: %s", esp_err_to_name(err));
            } else if (mode == TestMode::calibrate) {
                result = run_average(reader, mode, CALIBRATE_SAMPLES, cal_div, cal_offset);
            } else if (mode == TestMode::weight2time) {
                result = run_average(reader, mode, target, cal_div, cal_offset);
            } else {
                result = run_weight2inc(reader, target, inc_max_samples, cal_div, cal_offset);
            }
            ESP_LOGI(TAG, "Weight test stopped: state=%u, stat=%u,%u, time=%u ds, samples=%u, %" PRId64 " ms",
                     static_cast<unsigned>(result.state), result.stat_1, result.stat_2, result.time_ds,
                     static_cast<unsigned>(weight_test_data.sample_count),
                     (esp_timer_get_time() - started_us) / 1000);
        }

        // Wifi returns before the result is published, so a client that reads a result never finds wifi stopping.
        const esp_err_t wifi_on_err = ns->wifi_on(pdMS_TO_TICKS(WIFI_SWITCH_TIMEOUT_MS));
        if (wifi_on_err != ESP_OK) {
            ESP_LOGE(TAG, "Wifi start failed: %s", esp_err_to_name(wifi_on_err));
        } else {
            ESP_LOGI(TAG, "Wifi started");
        }

        xSemaphoreTake(test_mutex, portMAX_DELAY);
        weight_test_data.stat_1 = result.stat_1;
        weight_test_data.stat_2 = result.stat_2;
        weight_test_data.time_ds = result.time_ds;
        weight_test_data.test_state = result.state;
        test_running = false;
        xSemaphoreGive(test_mutex);
        led.set(wifi_on_err == ESP_OK ? StatusLed::Mode::on : StatusLed::Mode::off);  // on: ready for client
    }
}

static void stop_volume_test(TestState state, uint16_t time2fill_ds) {      // stop volume test task and update time2fill_ds
    xSemaphoreTake(test_mutex, portMAX_DELAY);
    volume_test_data.test_state = state;
    volume_test_data.time2fill_ds = time2fill_ds;
    test_running = false;
    xSemaphoreGive(test_mutex);
    led.set(StatusLed::Mode::on);                                           // wifi stayed on
}

static bool wait_for_stable_high(gpio_num_t gpio, uint16_t timeout_s,
                                 int64_t timeout_started_us,
                                 int64_t &high_started_us,
                                 uint16_t stable_time_ms,
                                 int64_t fill_started_us = 0)
{                                                                           // wait for a stable low-to-high transition
    const int64_t timeout_us = static_cast<int64_t>(timeout_s) * 1000000;
    bool low_seen = gpio_get_level(gpio) == 0;
    int64_t candidate_high_us = 0;
    TickType_t next_sample = xTaskGetTickCount();

    while (true) {
        vTaskDelayUntil(&next_sample, pdMS_TO_TICKS(VOLUME_GPIO_SAMPLE_PERIOD_MS));
        const int64_t now_us = esp_timer_get_time();
        const bool is_high = gpio_get_level(gpio) != 0;

        if (fill_started_us != 0) {                                         // client sees fill time while the test runs
            const uint16_t elapsed_ds = static_cast<uint16_t>(
                std::min<int64_t>((now_us - fill_started_us) / 100000, UINT16_MAX));
            xSemaphoreTake(test_mutex, portMAX_DELAY);
            volume_test_data.time2fill_ds = elapsed_ds;
            xSemaphoreGive(test_mutex);
        }

        if (!is_high) {
            low_seen = true;
            candidate_high_us = 0;
        } else if (low_seen) {
            if (candidate_high_us == 0) {
                candidate_high_us = now_us;
            } else if (now_us - candidate_high_us >= static_cast<int64_t>(stable_time_ms) * 1000) {
                high_started_us = candidate_high_us;
                return true;
            }
        }

        if (now_us - timeout_started_us >= timeout_us) {
            return false;
        }
    }
}

static void volume_test_task(void *arg) {
    (void)arg;

    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);                            // wait for reg_action signal

        xSemaphoreTake(test_mutex, portMAX_DELAY);
        const uint16_t timeout_low_s = volume_test_data.timeout_low_s;
        const uint16_t timeout_high_s = volume_test_data.timeout_high_s;
        const uint16_t stable_time_ms = volume_test_data.stable_time_ms;
        xSemaphoreGive(test_mutex);

        ESP_LOGI(TAG, "Volume test started: low timeout=%u s, high timeout=%u s", timeout_low_s, timeout_high_s);
        led.set(StatusLed::Mode::blink);

        const int64_t low_wait_started_us = esp_timer_get_time();
        int64_t low_edge_us = 0;
        if (!wait_for_stable_high(VOLUME_LOW_GPIO, timeout_low_s, low_wait_started_us, low_edge_us, stable_time_ms)) {
            stop_volume_test(TestState::timeout, 0);
            ESP_LOGI(TAG, "Volume test stopped: GPIO20 timeout");
            continue;
        }

        int64_t high_edge_us = 0;
        const bool high_edge_reached = wait_for_stable_high(
            VOLUME_HIGH_GPIO, timeout_high_s, low_edge_us, high_edge_us, stable_time_ms, low_edge_us);
        const int64_t stopped_us = high_edge_reached ? high_edge_us : esp_timer_get_time();
        const uint16_t elapsed_ds = static_cast<uint16_t>(
            std::min<int64_t>((stopped_us - low_edge_us) / 100000, UINT16_MAX));

        stop_volume_test(high_edge_reached ? TestState::done : TestState::timeout, elapsed_ds);
        ESP_LOGI(TAG, "Volume test stopped: %s after %u ds",
                 high_edge_reached ? "GPIO19 edge reached" : "GPIO19 timeout", elapsed_ds);
    }
}

static size_t readable_samples() {                                          // calibrate holds raw values, samples register words cannot carry them
    return weight_test_data.mode == TestMode::calibrate ? 0 : weight_test_data.sample_count;
}

static void copy_samples_segment(regs_action &areg_act) {
    const size_t first = areg_act.pointer[0];
    const size_t last = std::min<size_t>(first + REG_SAMPLES, readable_samples());
    memset(areg_act.samples, 0, sizeof(areg_act.samples));
    for (size_t i = first; i < last; i++) {
        areg_act.samples[i - first] = to_register(weight_test_data.samples[i]);
    }
}

static uint8_t start_test(const regs_action &areg_act) {                    // returns completion code, call with test_mutex taken
    if (areg_act.test[0] != TEST_START) {
        return 3;
    }
    if (test_running) {
        return 1;
    }
    const TestMode mode = static_cast<TestMode>(areg_act.mode[0]);
    const int16_t cal_div = static_cast<int16_t>(areg_act.config[4]);
    switch (mode) {
        case TestMode::weight2time:
            if (areg_act.target[0] < WEIGHT2TIME_MIN_SAMPLES || areg_act.target[0] > WEIGHT_TEST_MAX_SAMPLES || cal_div <= 0) {
                return 3;
            }
            break;
        case TestMode::weight2inc:
            if (areg_act.target[0] == 0 || cal_div <= 0) {
                return 3;
            }
            break;
        case TestMode::calibrate:
            break;
        case TestMode::volume:
            volume_test_data.test_state = TestState::pending;
            volume_test_data.time2fill_ds = 0;
            volume_test_data.timeout_low_s = areg_act.config[0];
            volume_test_data.timeout_high_s = areg_act.config[1];
            volume_test_data.stable_time_ms = areg_act.config[3];
            last_test = {static_cast<uint16_t>(mode), 0};
            test_running = true;
            xTaskNotifyGive(volume_test_task_handle);
            return 0;
        default:
            return 3;
    }
    weight_test_data.test_state = TestState::pending;
    weight_test_data.mode = mode;
    weight_test_data.target = areg_act.target[0];
    weight_test_data.inc_timeout_s = areg_act.config[2];
    weight_test_data.cal_div = cal_div;
    weight_test_data.cal_offset = static_cast<int16_t>(areg_act.config[5]);
    weight_test_data.stat_1 = 0;
    weight_test_data.stat_2 = 0;
    weight_test_data.time_ds = 0;
    weight_test_data.sample_count = 0;
    last_test = {static_cast<uint16_t>(mode), areg_act.target[0]};         // calibrate: target is the test weight, kept for get
    test_running = true;
    xTaskNotifyGive(weight_test_task_handle);
    return 0;
}

static void reg_action(regs_action &areg_act) {
    const int action_code = 16 * areg_act.func + areg_act.address;
    areg_act.ret_code = 0;
    xSemaphoreTake(test_mutex, portMAX_DELAY);
    switch (action_code) {
        case 0x60:                                                          // set mode
        case 0x61:                                                          // set target
        case 0x67:                                                          // set config
            if (test_running) {                                             // test uses its own copies, refused so the client knows
                areg_act.ret_code = 1;
            }
            break;

        case 0x62:                                                          // start test, weight task stops wifi after client disconnects
            areg_act.ret_code = start_test(areg_act);
            break;

        case 0x43:                                                          // client polls the volume test state
            areg_act.vol_stat[0] = static_cast<uint16_t>(volume_test_data.test_state);
            areg_act.vol_stat[1] = volume_test_data.time2fill_ds;
            break;

        case 0x44:                                                          // client reads the weight test state
            areg_act.weight_stat[0] = static_cast<uint16_t>(weight_test_data.test_state);
            areg_act.weight_stat[1] = weight_test_data.stat_1;
            areg_act.weight_stat[2] = weight_test_data.stat_2;
            areg_act.weight_stat[3] = static_cast<uint16_t>(readable_samples());
            areg_act.weight_stat[4] = weight_test_data.time_ds;
            break;

        case 0x48:                                                          // client reads which test get reports on
            areg_act.last_test[0] = last_test.mode;
            areg_act.last_test[1] = last_test.target;
            break;

        case 0x66:                                                          // pointer update, copy samples segment to samples register
            if (test_running) {
                areg_act.ret_code = 1;
            } else if (areg_act.pointer[0] >= readable_samples()) {
                areg_act.ret_code = 2;
            } else {
                copy_samples_segment(areg_act);
            }
            break;

        case 0x45:                                                          // client reads the segment copied by the pointer update
            break;

        default:
            areg_act.ret_code = 3;
            break;
    }
    xSemaphoreGive(test_mutex);
}

extern "C" void app_main(void)
{
    vTaskDelay(pdMS_TO_TICKS(2000));
    ESP_ERROR_CHECK(nvs_flash_init());

    test_mutex = xSemaphoreCreateMutex();
    configASSERT(test_mutex != nullptr);

    if (led.begin() != ESP_OK) {                                            // tests still work without the LED
        ESP_LOGE(TAG, "Status LED not started");
    }

    gpio_config_t volume_input_config = {};
    volume_input_config.pin_bit_mask = (1ULL << VOLUME_LOW_GPIO) | (1ULL << VOLUME_HIGH_GPIO);
    volume_input_config.mode = GPIO_MODE_INPUT;
    volume_input_config.pull_up_en = GPIO_PULLUP_ENABLE;
    volume_input_config.pull_down_en = GPIO_PULLDOWN_DISABLE;
    volume_input_config.intr_type = GPIO_INTR_DISABLE;
    ESP_ERROR_CHECK(gpio_config(&volume_input_config));

    QueueHandle_t q_protocol_to_main = xQueueCreate(10, sizeof(regs_action));
    QueueHandle_t q_main_to_protocol = xQueueCreate(10, sizeof(regs_action));
    configASSERT(q_protocol_to_main != nullptr);
    configASSERT(q_main_to_protocol != nullptr);
    static NetServer ns(q_main_to_protocol, q_protocol_to_main);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ns.wifi_init_ap();
    if (ncsi_responder_start() != ESP_OK) {                                 // tests still work, Windows may leave the AP
        ESP_LOGE(TAG, "NCSI responder not started");
    }
    ns.start_server_task();
    ns.start_protocol_task();

    const esp_err_t err = adc.begin();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ADS1231 init failed: %s", esp_err_to_name(err));
        return;
    }

    const BaseType_t task_created =
        xTaskCreate(weight_test_task, "weight_test_task", 4096, &ns, 5, &weight_test_task_handle);
    configASSERT(task_created == pdPASS);
    const BaseType_t volume_task_created =
        xTaskCreate(volume_test_task, "volume_test_task", 4096, nullptr, 5, &volume_test_task_handle);
    configASSERT(volume_task_created == pdPASS);

    const esp_err_t wifi_err = ns.wifi_on(pdMS_TO_TICKS(WIFI_SWITCH_TIMEOUT_MS));
    if (wifi_err != ESP_OK) {
        ESP_LOGE(TAG, "Wifi start failed: %s", esp_err_to_name(wifi_err));
    } else {
        ESP_LOGI(TAG, "Wifi started");
        led.set(StatusLed::Mode::on);                                       // ready for client
    }

    static regs_action main_reg_act;
    while (true) {
        xQueueReceive(q_protocol_to_main, &main_reg_act, portMAX_DELAY);
        reg_action(main_reg_act);
        xQueueSend(q_main_to_protocol, &main_reg_act, portMAX_DELAY);
    }
}
