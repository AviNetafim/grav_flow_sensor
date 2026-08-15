#include <algorithm>
#include <cstdint>
#include <cstdio>
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
#include "KeepCfg.h"
#include "NetServer.h"

static const char *SER_MAIN = "SER_MAIN";
static const char *TAG = "MAIN";

static constexpr size_t WEIGHT_TEST_MAX_SAMPLES = 1000;
static constexpr uint32_t WEIGHT_TEST_SAMPLE_PERIOD_MS = 100;                           // 100 ms sample period for weight test task
static constexpr int64_t WEIGHT_PRINT_PERIOD_US = 1000000;                              // TEMP DEBUG: print weight every 1 s

static constexpr int64_t CAL_MUL = 100;

static constexpr gpio_num_t VOLUME_LOW_GPIO = GPIO_NUM_20;
static constexpr gpio_num_t VOLUME_HIGH_GPIO = GPIO_NUM_19;
static constexpr uint32_t VOLUME_GPIO_SAMPLE_PERIOD_MS = 25;                            // 25 ms sample period for volume test task 

enum class TestState : uint16_t {
    stop = 0,
    run = 1,
};

enum class TestType : uint16_t {
    weight = 0,
    volume = 1,
    calibrate = 2,
};

struct WeightTestData {
    TestState test_state;
    uint16_t time2target_ds;
    uint16_t target_weight_10mg;                                        // target wegiht increment 
    uint16_t timeout_ds;                                                // timeout to target weight
    uint16_t cal_div;
    int16_t cal_offset;
    size_t sample_count;
    int32_t samples[WEIGHT_TEST_MAX_SAMPLES];
};

struct VolumeTestData {
    TestState test_state;
    uint16_t time2fill_ds;
    uint16_t timeout_low_ds;
    uint16_t timeout_high_ds;
    uint16_t stable_time_ms;
};

QueueHandle_t q_protocol_to_main;
QueueHandle_t q_main_to_protocol;
static ADS1231 adc(GPIO_NUM_17, GPIO_NUM_16, GPIO_NUM_18);
static KeepCfg kc;
static ProgramParameters work_params(0, 1800, 0, 0);
static regs_action main_reg_act;
static SemaphoreHandle_t ads1231_mutex;
static SemaphoreHandle_t weight_test_mutex;
static TaskHandle_t weight_test_task_handle;
static SemaphoreHandle_t volume_test_mutex;
static TaskHandle_t volume_test_task_handle;
static WeightTestData weight_test_data = {
    TestState::stop, 0, 0, 0, 12060, -4265, 0, {0}
};
static VolumeTestData volume_test_data = {
    TestState::stop, 0, 0, 0, 0
};

static bool read_ads1231(ADS1231::Sample &sample, TickType_t mutex_timeout) {
    if (xSemaphoreTake(ads1231_mutex, mutex_timeout) != pdTRUE) {
        return false;
    }

    esp_err_t err;
    while ((err = adc.poll(sample)) == ESP_ERR_NOT_FINISHED) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    xSemaphoreGive(ads1231_mutex);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ADS1231 read failed: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

static void stop_volume_test(uint16_t time2fill_ds) {                               // stop volume test task and update time2fill_ds 
    if (xSemaphoreTake(volume_test_mutex, portMAX_DELAY) == pdTRUE) {
        volume_test_data.test_state = TestState::stop;
        volume_test_data.time2fill_ds = time2fill_ds;
        xSemaphoreGive(volume_test_mutex);
    }
}

static bool wait_for_stable_high(gpio_num_t gpio, uint16_t timeout_ds,
                                 int64_t timeout_started_us,
                                 int64_t &high_started_us,
                                 uint16_t stable_time_ms,
                                 int64_t fill_started_us = 0)
{                                                                                  // wait for a stable low-to-high transition
    const int64_t timeout_us = static_cast<int64_t>(timeout_ds) * 100000;
    bool low_seen = gpio_get_level(gpio) == 0;
    int64_t candidate_high_us = 0;
    TickType_t next_sample = xTaskGetTickCount();                                  // rtos ticks since task was scheduled

    while (true) {
        vTaskDelayUntil(
            &next_sample, pdMS_TO_TICKS(VOLUME_GPIO_SAMPLE_PERIOD_MS));           // wait for next sample period  
        const int64_t now_us = esp_timer_get_time();                              // time since timer was initialized                                   
        const bool is_high = gpio_get_level(gpio) != 0;                           // read gpio level

        if (fill_started_us != 0) {                                               // if fill_started_us is provided, update time2fill_ds in volume_test_data  
            const uint16_t elapsed_ds = static_cast<uint16_t>(
                std::min<int64_t>((now_us - fill_started_us) / 100000,
                                  UINT16_MAX));
            if (xSemaphoreTake(volume_test_mutex, portMAX_DELAY) == pdTRUE) {
                volume_test_data.time2fill_ds = elapsed_ds;
                xSemaphoreGive(volume_test_mutex);
            }
        }

        if (!is_high) {
            low_seen = true;
            candidate_high_us = 0;
        } else if (low_seen) {
            if (candidate_high_us == 0) {
                candidate_high_us = now_us;
            } else if (now_us - candidate_high_us >=
                       static_cast<int64_t>(stable_time_ms) * 1000) {
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
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);                                    // wait for reg_action signal to start task

        uint16_t timeout_low_ds;
        uint16_t timeout_high_ds;
        uint16_t stable_time_ms;
        if (xSemaphoreTake(volume_test_mutex, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        timeout_low_ds = volume_test_data.timeout_low_ds;                           // update local copies of timeouts
        timeout_high_ds = volume_test_data.timeout_high_ds;
        stable_time_ms = volume_test_data.stable_time_ms;
        xSemaphoreGive(volume_test_mutex);

        ESP_LOGI(TAG, "Volume test started: low timeout=%u ds, high timeout=%u ds", 
                 timeout_low_ds, timeout_high_ds);

        const int64_t low_wait_started_us = esp_timer_get_time();
        int64_t low_edge_us = 0;
        if (!wait_for_stable_high(VOLUME_LOW_GPIO, timeout_low_ds,                  // wait for stable GPIO20 low-to-high
                                  low_wait_started_us, low_edge_us, stable_time_ms)) {
            stop_volume_test(0);                                                    // gpio20 timeout, stop test and set time2fill_ds to 0
            ESP_LOGI(TAG, "Volume test stopped: GPIO20 timeout");
            continue;
        }

        int64_t high_edge_us = 0;
        const bool high_edge_reached = wait_for_stable_high(                        // wait for stable GPIO19 low-to-high
            VOLUME_HIGH_GPIO, timeout_high_ds, low_edge_us, high_edge_us, stable_time_ms,
            low_edge_us);
        const int64_t stopped_us =                                                  // stop time is either GPIO19 edge or timeout 
            high_edge_reached ? high_edge_us : esp_timer_get_time();
        const uint16_t elapsed_ds = static_cast<uint16_t>(
            std::min<int64_t>((stopped_us - low_edge_us) / 100000,
                              UINT16_MAX));

        stop_volume_test(elapsed_ds);
        ESP_LOGI(TAG, "Volume test stopped: %s after %u ds",
                 high_edge_reached ? "GPIO19 edge reached" : "GPIO19 timeout",
                 elapsed_ds);
    }
}

static void weight_test_task(void *arg){
    (void)arg;

    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);                            //wait for reg_action signal 

        uint16_t target_weight_10mg;
        uint16_t timeout_ds;
        uint16_t cal_div;
        int16_t cal_offset;
        if (xSemaphoreTake(weight_test_mutex, portMAX_DELAY) != pdTRUE) {
            continue;                                                       // skip the loop if semaphore was not aqcuired    
        }
        target_weight_10mg = weight_test_data.target_weight_10mg;           // get local copies of target weight 
        timeout_ds = weight_test_data.timeout_ds;                           // and timeout                             
        cal_div = weight_test_data.cal_div;
        cal_offset = weight_test_data.cal_offset;
        xSemaphoreGive(weight_test_mutex);

        ADS1231::Sample adc_sample;                                         // read weight
        if (!read_ads1231(adc_sample, portMAX_DELAY)) {
            continue;
        }

        const int64_t tare_raw = static_cast<int64_t>(adc_sample.raw);      // update tare value (raw)
        ESP_LOGI(TAG, "Tare raw = %" PRId64 "" , tare_raw);
        const int64_t target_raw_0 = static_cast<int64_t>(-cal_offset * cal_div / CAL_MUL);
        ESP_LOGI(TAG, "target 0 = %" PRId64 "" , target_raw_0);
        const int64_t target_delta_raw_abs =                                    // target raw = target_10mg 
            static_cast<int64_t>(target_weight_10mg - cal_offset) * cal_div / CAL_MUL;  // target weight converted to raw units
        const int64_t target_delta_raw = target_delta_raw_abs - target_raw_0;
        ESP_LOGI(TAG, "target delta = %" PRId64 "" , target_delta_raw);
            const int64_t started_us = esp_timer_get_time();                // start_us (since timer was initialized)
        int64_t last_weight_print_us = started_us;                          // TEMP DEBUG
        TickType_t next_wake = xTaskGetTickCount();                         // rtos ticks since task was scheduled

        ESP_LOGI(TAG, "Weight test started: target = %u x 10 mg, timeout=%u ds",
                 target_weight_10mg, timeout_ds);

        while (true) {
            const int64_t elapsed_us = esp_timer_get_time() - started_us;   // time since test started in us 
            const uint16_t elapsed_ds = static_cast<uint16_t>(
                std::min<int64_t>(elapsed_us / 100000, UINT16_MAX));        // cap casted number to 2^16-1
            bool target_reached = false;

            bool sample_ready = false;
            if (xSemaphoreTake(ads1231_mutex, portMAX_DELAY) == pdTRUE) {
                const esp_err_t poll_err = adc.poll(adc_sample);
                sample_ready = poll_err == ESP_OK;
                if (poll_err != ESP_OK && poll_err != ESP_ERR_NOT_FINISHED) {
                    ESP_LOGE(TAG, "ADS1231 poll failed: %s",
                             esp_err_to_name(poll_err));
                }
                xSemaphoreGive(ads1231_mutex);
            }
            if (sample_ready) {
                const int32_t weight_10mg = static_cast<int32_t>(
                    (static_cast<int64_t>(adc_sample.raw) - tare_raw) * CAL_MUL / cal_div);  // convert raw to weight in 10 mg units

                // TEMP DEBUG: report the current sensor reading once per second.
                const int64_t now_us = esp_timer_get_time();
                if (now_us - last_weight_print_us >= WEIGHT_PRINT_PERIOD_US) {
                    ESP_LOGI(TAG,
                             "Weight sensor: raw=%ld, weight=%.2f g, "
                             "tare raw=%ld, target=%.2f g, target raw=%ld",
                             static_cast<long>(adc_sample.raw),
                             static_cast<double>(weight_10mg) / 100.0,
                             static_cast<long>(tare_raw),
                             static_cast<double>(target_weight_10mg) / 100.0,
                             static_cast<long>(target_delta_raw));
                    last_weight_print_us = now_us;
                }

                if (xSemaphoreTake(weight_test_mutex, portMAX_DELAY) == pdTRUE) {
                    if (weight_test_data.sample_count <                                
                        WEIGHT_TEST_MAX_SAMPLES) {
                        weight_test_data.samples[
                            weight_test_data.sample_count++] = weight_10mg;         // store reading in samples vector  
                    }
                    weight_test_data.time2target_ds = elapsed_ds;                   //  update time to target
                    xSemaphoreGive(weight_test_mutex);
                }

                target_reached =
                    static_cast<int64_t>(adc_sample.raw) - tare_raw >=
                    target_delta_raw;                                               // update target reached condition
            }

            const bool timed_out = elapsed_ds >= timeout_ds;                        // update timeout condition
            if (target_reached || timed_out) {
                if (xSemaphoreTake(weight_test_mutex, portMAX_DELAY) == pdTRUE) {
                    weight_test_data.test_state = TestState::stop;       
                weight_test_data.time2target_ds = elapsed_ds;                       // continuously done above?
                    xSemaphoreGive(weight_test_mutex);
                }
                ESP_LOGI(TAG, "Weight test stop: %s after %u ds",
                         target_reached ? "target reached" : "timeout",
                         elapsed_ds);                                            
                break;
            }

            vTaskDelayUntil(&next_wake,
                            pdMS_TO_TICKS(WEIGHT_TEST_SAMPLE_PERIOD_MS));
        }
    }
}

void show_reg_act(regs_action areg_act){

	// show register states after client before and after ommand received, add configuration parameter to show registers (!)
    if (areg_act.config[9] == 1){
        int i;
        printf("\nfunction = %d, address: = %d \n", areg_act.func,areg_act.address);	 // decoded required function and address
        printf(" -- mode :"); printf("%04x \n", areg_act.mode[0]);
        printf(" -- target weight : %04x \n", areg_act.target_weight[0]); 
        printf(" -- test:"); printf("%04x \n", areg_act.test[0]);
        printf(" -- vol test stat:");
        for (i = 0 ; i < 2 ; i ++)	printf(" %04x", areg_act.vol_stat[i]);
        printf("\n");     
        printf(" -- weight test stat:");
        for (i = 0 ; i < 3 ; i ++)	printf(" %04x", areg_act.weight_stat[i]);
        printf("\n");     
        printf(" -- samples:");
        for (i = 0 ; i < REG_SAMPLES ; i ++) printf(" %04x", areg_act.samples[i]);
        printf("\n");     
        printf(" -- actuator:");
        for (i = 0 ; i < 2 ; i ++)	printf(" %04x", areg_act.actuator[i]);
        printf("\n");     
        printf(" -- actuate:"); printf("%04x \n", areg_act.actuate[0]);
        printf(" -- config:");
        for (i = 0; i < 10; i++) printf(" %04x", areg_act.config[i]);
        printf("\n");
    }
}

void reg_action(regs_action &areg_act)
{
    const int action_code = 10 * areg_act.func + areg_act.address;
    areg_act.ret_code = 0;
    printf("reg_action: action_code=%d\n", action_code);
    switch (action_code) {
        case 60:                                                                    // set measurement mode (weight/volume)
            if (areg_act.mode[0] == static_cast<uint16_t>(TestType::weight) ||
                areg_act.mode[0] == static_cast<uint16_t>(TestType::volume) ||
                areg_act.mode[0] == static_cast<uint16_t>(TestType::calibrate)) {
                areg_act.ret_code = 0;
            } else {
                areg_act.ret_code = 3;                                              // invalid mode
            }
        break;
        case 62:                                                                    // start test (weight/volume)
            printf("reg_action: test command received, mode=%u, test=%u\n", areg_act.mode[0], areg_act.test[0]);
            if (areg_act.test[0] == static_cast<uint16_t>(TestState::run)){  
                areg_act.ret_code = 0;       
                switch (static_cast<TestType>(areg_act.mode[0])){
                    case TestType::weight:                                          // start weight test task
                        if (xSemaphoreTake(weight_test_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                            if (areg_act.config[4] == 0) {
                                areg_act.ret_code = 3;
                                xSemaphoreGive(weight_test_mutex);
                                break;
                            }
                            if (weight_test_data.test_state == TestState::run) {
                                xSemaphoreGive(weight_test_mutex);
                                break;
                            }
                            weight_test_data.test_state = TestState::run;
                            weight_test_data.time2target_ds = 0;
                            weight_test_data.target_weight_10mg = areg_act.target_weight[0];
                            weight_test_data.timeout_ds = areg_act.config[2];
                            weight_test_data.cal_div = areg_act.config[4];
                            weight_test_data.cal_offset = static_cast<int16_t>(areg_act.config[5]);
                            weight_test_data.sample_count = 0;
                            memset(weight_test_data.samples, 0, sizeof(weight_test_data.samples)); // clear samples buffer
                            xSemaphoreGive(weight_test_mutex);
                            xTaskNotifyGive(weight_test_task_handle);               // start weight test task 
                        }
                        else areg_act.ret_code = 1;                                // cannot access data structure to start the test when retcode = 0 (!)    
                    break;
                
                    case TestType::volume:
                        if (xSemaphoreTake(volume_test_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                            if (volume_test_data.test_state == TestState::run) {    
                                xSemaphoreGive(volume_test_mutex);
                                break;
                            }
                            volume_test_data.test_state = TestState::run;
                            volume_test_data.time2fill_ds = 0;
                            volume_test_data.timeout_low_ds = areg_act.config[0];
                            volume_test_data.timeout_high_ds = areg_act.config[1];
                            volume_test_data.stable_time_ms = areg_act.config[3];
                            xSemaphoreGive(volume_test_mutex);
                            xTaskNotifyGive(volume_test_task_handle);               // start volume test task
                        }
                        else areg_act.ret_code = 1;                                 //
                    break;

                    case TestType::calibrate: {
                        printf("reg_action: calibration command received\n");
                        ADS1231::Sample adc_sample;
                        if (read_ads1231(adc_sample, pdMS_TO_TICKS(50))) {
                            printf("Calibration raw reading: %ld\n", adc_sample.raw);
                            const uint32_t raw = static_cast<uint32_t>(adc_sample.raw);
                            areg_act.samples[0] = static_cast<uint16_t>(raw & 0xFFFF);
                            areg_act.samples[1] = static_cast<uint16_t>(raw >> 16);
                        } else {
                            printf("Calibration failed: could not read from ADS1231\n");
                            areg_act.ret_code = 1;
                        }
                    }
                    break;

                    default:
                        areg_act.ret_code = 3;                                      // invalid mode for test start
                    break;
                }    
            }
            else areg_act.ret_code = 3;                                             // invalid test command
        break;

        case 43:                                                                    // client polls the volume-test state
            if (xSemaphoreTake(volume_test_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                areg_act.vol_stat[0] = static_cast<uint16_t>(volume_test_data.test_state);
                areg_act.vol_stat[1] = volume_test_data.time2fill_ds;
                xSemaphoreGive(volume_test_mutex);
            } 
            else areg_act.ret_code = 1;
        break;

        case 44:                                                                    // client polls the weight-test state
            if (xSemaphoreTake(weight_test_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                areg_act.weight_stat[0] = static_cast<uint16_t>(weight_test_data.test_state);
                areg_act.weight_stat[1] = static_cast<uint16_t>(weight_test_data.time2target_ds);
                areg_act.weight_stat[2] = static_cast<uint16_t>(weight_test_data.sample_count);
                xSemaphoreGive(weight_test_mutex);
            } 
            else areg_act.ret_code = 1;
        break;

        case 45:                                                                     //  copy sample buffer segment to samples buffer register
        if (areg_act.mode[0] == static_cast<uint16_t>(TestType::weight)) {                                  // only valid for weight test mode
            if (xSemaphoreTake(weight_test_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                for (int i = 0  ; i < REG_SAMPLES ; i++){
                    areg_act.samples[i] = weight_test_data.samples[areg_act.pointer[0]+i];
                }
                xSemaphoreGive(weight_test_mutex);
            }
            else areg_act.ret_code = 1;

        }
        break;
        case 68:                                                                     //  actuate selected valve 
        // set gpio  to 0/1 per areg_act.actuator[0,1]
        break;

    default:
        areg_act.ret_code = 0;
        break;
    }
}

extern "C" void app_main(void)
{
    vTaskDelay(pdMS_TO_TICKS(2000));
    ESP_ERROR_CHECK(nvs_flash_init());
    kc.init(work_params);
    kc.load_parameters(work_params);
    ESP_LOGI(SER_MAIN, ",boot log_switch=%u", work_params.log_switch);
    ESP_LOGI(SER_MAIN, ",boot log_period=%u", work_params.log_period);

    ads1231_mutex = xSemaphoreCreateMutex();
    configASSERT(ads1231_mutex != nullptr);
    weight_test_mutex = xSemaphoreCreateMutex();
    configASSERT(weight_test_mutex != nullptr);
    volume_test_mutex = xSemaphoreCreateMutex();
    configASSERT(volume_test_mutex != nullptr);

    gpio_config_t volume_input_config = {};
    volume_input_config.pin_bit_mask =
        (1ULL << VOLUME_LOW_GPIO) | (1ULL << VOLUME_HIGH_GPIO);
    volume_input_config.mode = GPIO_MODE_INPUT;
    volume_input_config.pull_up_en = GPIO_PULLUP_ENABLE;
    volume_input_config.pull_down_en = GPIO_PULLDOWN_DISABLE;
    volume_input_config.intr_type = GPIO_INTR_DISABLE;
    ESP_ERROR_CHECK(gpio_config(&volume_input_config));

    q_protocol_to_main = xQueueCreate(10, sizeof(regs_action));
    q_main_to_protocol = xQueueCreate(10, sizeof(regs_action));
    configASSERT(q_protocol_to_main != nullptr);
    configASSERT(q_main_to_protocol != nullptr);
    static NetServer ns(q_main_to_protocol, q_protocol_to_main);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ns.wifi_init_ap();
    ns.start_server_task();
    ns.start_protocol_task();

    const esp_err_t err = adc.begin();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ADS1231 init failed: %s", esp_err_to_name(err));
        return;
    }

    const BaseType_t task_created =
        xTaskCreate(weight_test_task, "weight_test_task", 4096, nullptr, 5,
                    &weight_test_task_handle);
    configASSERT(task_created == pdPASS);
    const BaseType_t volume_task_created =
        xTaskCreate(volume_test_task, "volume_test_task", 4096, nullptr, 5,
                    &volume_test_task_handle);
    configASSERT(volume_task_created == pdPASS);

    while (true) {
        xQueueReceive(q_protocol_to_main, &main_reg_act, portMAX_DELAY);
        show_reg_act(main_reg_act);
        reg_action(main_reg_act);
        show_reg_act(main_reg_act);
        xQueueSend(q_main_to_protocol, &main_reg_act, portMAX_DELAY);
    }
}

