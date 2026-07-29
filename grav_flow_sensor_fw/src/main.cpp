#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "esp_log.h"
#include "esp_timer.h"
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
static constexpr uint32_t WEIGHT_TEST_SAMPLE_PERIOD_MS = 100;
static constexpr uint16_t WEIGHT_TEST_TIMEOUT_DS = 900; // 90 s, must be < 100 s
static constexpr int64_t CAL_NUM = 12049;
static constexpr int64_t CAL_DEN = 100;

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

QueueHandle_t q_protocol_to_main;
QueueHandle_t q_main_to_protocol;
static ADS1231 adc(GPIO_NUM_17, GPIO_NUM_16);
static KeepCfg kc;
static ProgramParameters work_params(0, 1800, 0, 0);
static regs_action main_reg_act;
static SemaphoreHandle_t weight_test_mutex;
static TaskHandle_t weight_test_task_handle;
static WeightTestData weight_test_data = {
    TestState::stop, 0, 0, WEIGHT_TEST_TIMEOUT_DS, 0, {0}
};

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
	printf("function = %d, address: = %d \n", areg_act.func,areg_act.address);	 // decoded required function and address
	printf(" -- mode :"); printf("%04x \n", areg_act.mode[0]);
	printf(" -- target weight : %04x", areg_act.target_weight[0]); 
	printf(" -- test:"); printf("%04x \n", areg_act.test[0]);
	printf(" -- vol test stat:");
    for (i = 0 ; i < 2 ; i ++)	printf(" %04x", areg_act.vol_stat[i]);
    printf("\n");     
    printf(" -- weight test stat:");
    for (i = 0 ; i < 3 ; i ++)	printf(" %04x", areg_act.weight_stat[i]);
    printf("\n");     
	printf(" -- samples:");
	for (i = 0 ; i < REG_SAMPLES ; i ++)	printf(" %04x", areg_act.samples[i]);
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
        case 62:                                                                // start test (weight/volume)
            if (areg_act.test[0] == static_cast<uint16_t>(TestState::run)){
                switch (static_cast<TestType>(areg_act.mode[0])){
                    case TestType::weight:                                      // start weight test task
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
                
                    case TestType:: volume:
                    break;
                }    
            }
        break;

        case 43:                                                                    // client polls the volume-test state
            if (xSemaphoreTake(volume_test_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                areg_act.vol_stat[0] = static_cast<uint16_t>(volume_test_data.test_state);
                areg_act.vol_stat[1] = static_cast<uint16_t>(volume_test_data.time2target_ds);
                xSemaphoreGive(weight_test_mutex);
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
        areg_act.ret_code = 9;
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

    while (true) {
        xQueueReceive(q_protocol_to_main, &main_reg_act, portMAX_DELAY);
        show_reg_act(main_reg_act);
        reg_action(main_reg_act);
        xQueueSend(q_main_to_protocol, &main_reg_act, portMAX_DELAY);
    }
}
