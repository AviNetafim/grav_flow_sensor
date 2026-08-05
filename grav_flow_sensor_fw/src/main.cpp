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
static constexpr int64_t WEIGHT_PRINT_PERIOD_US = 3000000;                              // 3 s print period for weight test task     
static constexpr uint16_t WEIGHT_TEST_TIMEOUT_DS = 900; // 90 s, must be < 100 s
static constexpr int64_t CAL_NUM = 12049;                                               // calibration factors for 10 mg units, derived from calibration with known weight
static constexpr int64_t CAL_DEN = 100;                                                 
static constexpr gpio_num_t VOLUME_LOW_GPIO = GPIO_NUM_20;
static constexpr gpio_num_t VOLUME_HIGH_GPIO = GPIO_NUM_19;
static constexpr uint16_t VOLUME_TIMEOUT_LOW_DS = 200;                                  // 10s to start filling, must be < 100s 
static constexpr uint16_t VOLUME_TIMEOUT_HIGH_DS = 600;                                 // 60s to fill, must be < 100s    
static constexpr uint32_t VOLUME_GPIO_SAMPLE_PERIOD_MS = 25;                            // 25 ms sample period for volume test task 
static constexpr int64_t VOLUME_GPIO_STABLE_LOW_US = 100000;                            // 100 ms stable low to detect edge 

enum class TestState : uint16_t {
    stop = 0,
    run = 1,
};

enum class TestType : uint16_t {
    weight = 0,
    volume = 1,
};

struct WeightTestData {
    TestState test_state;
    uint16_t time2target_ds;
    uint16_t target_weight_10mg;                                        // target wegiht increment 
    uint16_t timeout_ds;                                                // timeout to target weight
    size_t sample_count;
    int32_t samples[WEIGHT_TEST_MAX_SAMPLES];
};

struct VolumeTestData {
    TestState test_state;
    uint16_t time2fill_ds;
    uint16_t timeout_low_ds;
    uint16_t timeout_high_ds;
};

QueueHandle_t q_protocol_to_main;
QueueHandle_t q_main_to_protocol;
static ADS1231 adc(GPIO_NUM_17, GPIO_NUM_16);
static KeepCfg kc;
static ProgramParameters work_params(0, 1800, 0, 0);
static regs_action main_reg_act;
static SemaphoreHandle_t weight_test_mutex;
static TaskHandle_t weight_test_task_handle;
static SemaphoreHandle_t volume_test_mutex;
static TaskHandle_t volume_test_task_handle;
static WeightTestData weight_test_data = {
    TestState::stop, 0, 0, WEIGHT_TEST_TIMEOUT_DS, 0, {0}
};
static VolumeTestData volume_test_data = {
    TestState::stop, 0, VOLUME_TIMEOUT_LOW_DS, VOLUME_TIMEOUT_HIGH_DS
};

static void stop_volume_test(uint16_t time2fill_ds) {                               // stop volume test task and update time2fill_ds 
    if (xSemaphoreTake(volume_test_mutex, portMAX_DELAY) == pdTRUE) {
        volume_test_data.test_state = TestState::stop;
        volume_test_data.time2fill_ds = time2fill_ds;
        xSemaphoreGive(volume_test_mutex);
    }
}

static bool wait_for_stable_low(gpio_num_t gpio, uint16_t timeout_ds,
                                int64_t timeout_started_us,
                                int64_t &low_started_us,
                                int64_t fill_started_us = 0) 
{                                                                                  // wait for gpio to go low and stay low for at least 100 ms
    const int64_t timeout_us = static_cast<int64_t>(timeout_ds) * 100000;
    bool high_seen = gpio_get_level(gpio) != 0;
    int64_t candidate_low_us = 0;
    TickType_t next_sample = xTaskGetTickCount();                                  // rtos ticks since task was scheduled

    while (true) {
        vTaskDelayUntil(
            &next_sample, pdMS_TO_TICKS(VOLUME_GPIO_SAMPLE_PERIOD_MS));           // wait for next sample period  
        const int64_t now_us = esp_timer_get_time();                              // time since timer was initialized                                   
        const bool is_low = gpio_get_level(gpio) == 0;                            // read gpio level  

        if (fill_started_us != 0) {                                               // if fill_started_us is provided, update time2fill_ds in volume_test_data  
            const uint16_t elapsed_ds = static_cast<uint16_t>(
                std::min<int64_t>((now_us - fill_started_us) / 100000,
                                  UINT16_MAX));
            if (xSemaphoreTake(volume_test_mutex, portMAX_DELAY) == pdTRUE) {
                volume_test_data.time2fill_ds = elapsed_ds;
                xSemaphoreGive(volume_test_mutex);
            }
        }

        if (!is_low) {
            high_seen = true;
            candidate_low_us = 0;
        } else if (high_seen) {
            if (candidate_low_us == 0) {
                candidate_low_us = now_us;
            } else if (now_us - candidate_low_us >=
                       VOLUME_GPIO_STABLE_LOW_US) {
                low_started_us = candidate_low_us;
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
        if (xSemaphoreTake(volume_test_mutex, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        timeout_low_ds = volume_test_data.timeout_low_ds;                           // update local copies of timeouts
        timeout_high_ds = volume_test_data.timeout_high_ds;
        xSemaphoreGive(volume_test_mutex);

        ESP_LOGI(TAG, "Volume test started: low timeout=%u ds, high timeout=%u ds", 
                 timeout_low_ds, timeout_high_ds);

        const int64_t low_wait_started_us = esp_timer_get_time();
        int64_t low_edge_us = 0;
        if (!wait_for_stable_low(VOLUME_LOW_GPIO, timeout_low_ds,                   // wait for GPIO20 to go low for at least 100 ms 
                                 low_wait_started_us, low_edge_us)) {
            stop_volume_test(0);                                                    // gpio20 timeout, stop test and set time2fill_ds to 0
            ESP_LOGI(TAG, "Volume test stopped: GPIO20 timeout");
            continue;
        }

        int64_t high_edge_us = 0;
        const bool high_edge_reached = wait_for_stable_low(                         // wait for GPIO19 to go low for at least 100 ms      
            VOLUME_HIGH_GPIO, timeout_high_ds, low_edge_us, high_edge_us,
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
        if (xSemaphoreTake(weight_test_mutex, portMAX_DELAY) != pdTRUE) {
            continue;                                                       // skip the loop if semaphore was not aqcuired    
        }
        target_weight_10mg = weight_test_data.target_weight_10mg;           // get local copies of target weight 
        timeout_ds = weight_test_data.timeout_ds;                           // and timeout                             
        xSemaphoreGive(weight_test_mutex);

        ADS1231::Sample adc_sample;                                         // read weight 
        while (!adc.poll(adc_sample)) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }

        const int32_t tare_raw = adc_sample.raw;                            // update tare value (raw)
        const int64_t target_delta_raw =                                    // and target increment in raw 
            static_cast<int64_t>(target_weight_10mg) * CAL_NUM / CAL_DEN;
        const int64_t started_us = esp_timer_get_time();                    // start_us (since timer was initialized)
        int64_t last_weight_print_us = started_us;
        TickType_t next_wake = xTaskGetTickCount();                         // rtos ticks since task was scheduled

        ESP_LOGI(TAG, "Weight test started: target=%u x 10 mg, timeout=%u ds",
                 target_weight_10mg, timeout_ds);

        while (true) {
            const int64_t elapsed_us = esp_timer_get_time() - started_us;   // time since test started in us 
            const uint16_t elapsed_ds = static_cast<uint16_t>(
                std::min<int64_t>(elapsed_us / 100000, UINT16_MAX));        // cap casted number to 2^16-1
            bool target_reached = false;

            if (adc.poll(adc_sample)) {
                const int32_t weight_10mg = static_cast<int32_t>(
                    (static_cast<int64_t>(adc_sample.raw) - tare_raw) * CAL_DEN / CAL_NUM);                    

                const int64_t now_us = esp_timer_get_time();
                if (now_us - last_weight_print_us >= WEIGHT_PRINT_PERIOD_US) {
                    ESP_LOGI(TAG, "Measured weight: %.2f g",
                             static_cast<double>(weight_10mg) / 100.0);
                    last_weight_print_us = now_us;
                }

                if (xSemaphoreTake(weight_test_mutex, portMAX_DELAY) == pdTRUE) {
                    if (weight_test_data.sample_count <                                
                        WEIGHT_TEST_MAX_SAMPLES) {
                        weight_test_data.samples[
                            weight_test_data.sample_count++] = weight_10mg;  // store reading in samples
                    }
                    weight_test_data.time2target_ds = elapsed_ds;            //  update time to target
                    xSemaphoreGive(weight_test_mutex);
                }

                target_reached =
                    static_cast<int64_t>(adc_sample.raw) - tare_raw >=
                    target_delta_raw;                                        // update target reached condition
            }

            const bool timed_out = elapsed_ds >= timeout_ds;                // update timeout condition
            if (target_reached || timed_out) {
                if (xSemaphoreTake(weight_test_mutex, portMAX_DELAY) == pdTRUE) {
                    weight_test_data.test_state = TestState::stop;       
                    weight_test_data.time2target_ds = elapsed_ds;           // continuously done above?
                    xSemaphoreGive(weight_test_mutex);
                }
                ESP_LOGI(TAG, "Weight test stop: %s after %u ds",
                         target_reached ? "target reached" : "timeout",
                         elapsed_ds);                                       //     
                break;
            }

            vTaskDelayUntil(&next_wake,
                            pdMS_TO_TICKS(WEIGHT_TEST_SAMPLE_PERIOD_MS));
        }
    }
}

void show_reg_act(regs_action areg_act){

	// show register states after client before and after ommand received, add configuration parameter to show registers (!)
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
}

void reg_action(regs_action &areg_act)
{
    const int action_code = 10 * areg_act.func + areg_act.address;
    areg_act.ret_code = 0;

    switch (action_code) {
        case 60:                                                                    // set measurement mode (weight/volume)
            if (areg_act.mode[0] == static_cast<uint16_t>(TestType::weight) ||
                areg_act.mode[0] == static_cast<uint16_t>(TestType::volume)) {
                areg_act.ret_code = 0;
            } else {
                areg_act.ret_code = 3;                                              // invalid mode
            }
        break;
        case 61:                                                                    // set target weight (10mg units)
            areg_act.ret_code = 0;
        break;
        case 62:                                                                    // start test (weight/volume)
            if (areg_act.test[0] == static_cast<uint16_t>(TestState::run)){  
                areg_act.ret_code = 0;       
                switch (static_cast<TestType>(areg_act.mode[0])){
                    case TestType::weight:                                          // start weight test task
                        if (xSemaphoreTake(weight_test_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                            if (weight_test_data.test_state == TestState::run) {
                                areg_act.ret_code = 2;                              // task already run
                                xSemaphoreGive(weight_test_mutex);
                                break;
                            }
                            weight_test_data.test_state = TestState::run;
                            weight_test_data.time2target_ds = 0;
                            weight_test_data.target_weight_10mg = areg_act.target_weight[0];
                            weight_test_data.timeout_ds = WEIGHT_TEST_TIMEOUT_DS;
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
                                areg_act.ret_code = 2;
                                xSemaphoreGive(volume_test_mutex);
                                break;
                            }
                            volume_test_data.test_state = TestState::run;
                            volume_test_data.time2fill_ds = 0;
                            volume_test_data.timeout_low_ds = VOLUME_TIMEOUT_LOW_DS;
                            volume_test_data.timeout_high_ds = VOLUME_TIMEOUT_HIGH_DS;
                            xSemaphoreGive(volume_test_mutex);
                            xTaskNotifyGive(volume_test_task_handle);               // start volume test task
                        }
                        else areg_act.ret_code = 1;                                 //
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
            if (xSemaphoreTake(weight_test_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                for (int i = 0  ; i < REG_SAMPLES ; i++){
                    areg_act.samples[i] = weight_test_data.samples[areg_act.pointer[0]+i];
                }
                xSemaphoreGive(weight_test_mutex);
            }
            else areg_act.ret_code = 1;
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
