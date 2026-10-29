#include <cstdint>
#include <cstdio>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "ads1231.h"
#include "NetServer.h"

static constexpr char TAG[] = "ADC_STABILITY";
static constexpr uint16_t MIN_SAMPLE_PERIOD_MS = 100;
static constexpr uint16_t MAX_SAMPLE_PERIOD_MS = 1000;
static constexpr uint16_t MIN_SAMPLE_COUNT = 10;
static constexpr uint16_t MAX_SAMPLE_COUNT = 1000;
static constexpr uint32_t ADC_READ_TIMEOUT_MS = 1000;

enum class TestState : uint16_t {
    stop = 0,
    run = 1,
};

enum class TestType : uint16_t {
    weight = 0,
};

struct WeightTestData {
    TestState test_state;
    uint16_t samples_completed;
    uint16_t sample_count;
    uint16_t sample_period_ms;
};

QueueHandle_t q_protocol_to_main;
QueueHandle_t q_main_to_protocol;

static ADS1231 adc(GPIO_NUM_17, GPIO_NUM_16, GPIO_NUM_18);
static regs_action main_reg_act;
static SemaphoreHandle_t ads1231_mutex;
static SemaphoreHandle_t weight_test_mutex;
static TaskHandle_t weight_test_task_handle;
static WeightTestData weight_test_data = {
    TestState::stop, 0, MIN_SAMPLE_COUNT, MIN_SAMPLE_PERIOD_MS
};

static bool read_ads1231(ADS1231::Sample &sample)
{
    if (xSemaphoreTake(ads1231_mutex, pdMS_TO_TICKS(ADC_READ_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "Timed out waiting for the ADS1231 mutex");
        return false;
    }

    const int64_t deadline_us =
        esp_timer_get_time() + static_cast<int64_t>(ADC_READ_TIMEOUT_MS) * 1000;
    esp_err_t err;
    do {
        err = adc.poll(sample);
        if (err == ESP_ERR_NOT_FINISHED) {
            if (esp_timer_get_time() >= deadline_us) {
                ESP_LOGE(TAG, "Timed out waiting for an ADS1231 conversion");
                xSemaphoreGive(ads1231_mutex);
                return false;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    } while (err == ESP_ERR_NOT_FINISHED);

    xSemaphoreGive(ads1231_mutex);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ADS1231 read failed: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

static void weight_test_task(void *arg)
{
    (void)arg;

    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        uint16_t sample_count;
        uint16_t sample_period_ms;
        if (xSemaphoreTake(weight_test_mutex, portMAX_DELAY) != pdTRUE) {
            ESP_LOGE(TAG, "Could not lock weight-test data");
            continue;
        }
        sample_count = weight_test_data.sample_count;
        sample_period_ms = weight_test_data.sample_period_ms;
        xSemaphoreGive(weight_test_mutex);

        ESP_LOGI(TAG, "Raw weight test started: count=%u period=%u ms",
                 sample_count, sample_period_ms);

        TickType_t next_wake = xTaskGetTickCount();
        bool read_failed = false;
        uint16_t completed_samples = 0;
        for (uint16_t i = 0; i < sample_count; ++i) {
            ADS1231::Sample sample{};
            if (!read_ads1231(sample)) {
                read_failed = true;
                break;
            }

            printf("weight_test sample=%u/%u raw=%ld\r\n",
                   static_cast<unsigned int>(i + 1),
                   static_cast<unsigned int>(sample_count),
                   static_cast<long>(sample.raw));

            if (xSemaphoreTake(weight_test_mutex, portMAX_DELAY) != pdTRUE) {
                ESP_LOGE(TAG, "Could not lock weight-test progress");
                read_failed = true;
                break;
            }
            weight_test_data.samples_completed = static_cast<uint16_t>(i + 1);
            completed_samples = weight_test_data.samples_completed;
            xSemaphoreGive(weight_test_mutex);

            if (i + 1 < sample_count) {
                vTaskDelayUntil(&next_wake, pdMS_TO_TICKS(sample_period_ms));
            }
        }

        if (xSemaphoreTake(weight_test_mutex, portMAX_DELAY) == pdTRUE) {
            weight_test_data.test_state = TestState::stop;
            xSemaphoreGive(weight_test_mutex);
        } else {
            ESP_LOGE(TAG, "Could not lock weight-test state after sampling");
        }

        ESP_LOGI(TAG, "Raw weight test %s: %u/%u samples",
                 read_failed ? "stopped after read error" : "completed",
                 static_cast<unsigned int>(completed_samples),
                 static_cast<unsigned int>(sample_count));
    }
}

void show_reg_act(regs_action areg_act)
{
    if (areg_act.config[9] != 1) {
        return;
    }

    printf("\nfunction=%u address=%u\n",
           static_cast<unsigned int>(areg_act.func),
           static_cast<unsigned int>(areg_act.address));
    printf(" -- mode: %04x\n", areg_act.mode[0]);
    printf(" -- target weight: %04x\n", areg_act.target_weight[0]);
    printf(" -- test: %04x\n", areg_act.test[0]);
    printf(" -- volume status:");
    for (size_t i = 0; i < 2; ++i) {
        printf(" %04x", areg_act.vol_stat[i]);
    }
    printf("\n");
    printf(" -- weight status:");
    for (size_t i = 0; i < REG_WEIGHT_STAT; ++i) {
        printf(" %04x", areg_act.weight_stat[i]);
    }
    printf("\n -- samples:");
    for (size_t i = 0; i < REG_SAMPLES; ++i) {
        printf(" %04x",
               static_cast<unsigned int>(
                   static_cast<uint16_t>(areg_act.samples[i])));
    }
    printf("\n -- sample pointer: %04x\n", areg_act.pointer[0]);
    printf(" -- actuator:");
    for (size_t i = 0; i < 2; ++i) {
        printf(" %04x", areg_act.actuator[i]);
    }
    printf("\n -- actuate: %04x\n", areg_act.actuate[0]);
    printf(" -- config:");
    for (size_t i = 0; i < 10; ++i) {
        printf(" %04x", areg_act.config[i]);
    }
    printf("\n");
}

void reg_action(regs_action &areg_act)
{
    const unsigned int action_code =
        16U * static_cast<unsigned int>(areg_act.func) + areg_act.address;
    areg_act.ret_code = 0;
    printf("reg_action: action_code=0x%02X\n", action_code);

    switch (action_code) {
    case 0x60:
        if (areg_act.mode[0] != static_cast<uint16_t>(TestType::weight)) {
            areg_act.ret_code = 3;
        }
        break;

    case 0x62:
        if (areg_act.test[0] != static_cast<uint16_t>(TestState::run) ||
            areg_act.mode[0] != static_cast<uint16_t>(TestType::weight)) {
            areg_act.ret_code = 3;
            break;
        }
        if (areg_act.config[7] < MIN_SAMPLE_PERIOD_MS ||
            areg_act.config[7] > MAX_SAMPLE_PERIOD_MS ||
            areg_act.config[8] < MIN_SAMPLE_COUNT ||
            areg_act.config[8] > MAX_SAMPLE_COUNT) {
            printf("Invalid sample configuration: period=%u ms count=%u\n",
                   static_cast<unsigned int>(areg_act.config[7]),
                   static_cast<unsigned int>(areg_act.config[8]));
            areg_act.ret_code = 3;
            break;
        }
        if (xSemaphoreTake(weight_test_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
            areg_act.ret_code = 1;
            break;
        }
        if (weight_test_data.test_state == TestState::run) {
            xSemaphoreGive(weight_test_mutex);
            break;
        }
        weight_test_data.test_state = TestState::run;
        weight_test_data.samples_completed = 0;
        weight_test_data.sample_count = areg_act.config[8];
        weight_test_data.sample_period_ms = areg_act.config[7];
        xSemaphoreGive(weight_test_mutex);
        xTaskNotifyGive(weight_test_task_handle);
        break;

    case 0x44:
        if (xSemaphoreTake(weight_test_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
            areg_act.ret_code = 1;
            break;
        }
        areg_act.weight_stat[0] =
            static_cast<uint16_t>(weight_test_data.test_state);
        areg_act.weight_stat[1] = weight_test_data.samples_completed;
        areg_act.weight_stat[2] = weight_test_data.sample_count;
        areg_act.weight_stat[3] = weight_test_data.sample_period_ms;
        xSemaphoreGive(weight_test_mutex);
        break;

    default:
        areg_act.ret_code = 3;
        break;
    }
}

extern "C" void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    ads1231_mutex = xSemaphoreCreateMutex();
    weight_test_mutex = xSemaphoreCreateMutex();
    configASSERT(ads1231_mutex != nullptr);
    configASSERT(weight_test_mutex != nullptr);

    q_protocol_to_main = xQueueCreate(10, sizeof(regs_action));
    q_main_to_protocol = xQueueCreate(10, sizeof(regs_action));
    configASSERT(q_protocol_to_main != nullptr);
    configASSERT(q_main_to_protocol != nullptr);

    static NetServer ns(q_main_to_protocol, q_protocol_to_main);
    ns.wifi_init_ap();
    ns.start_server_task();
    ns.start_protocol_task();

    ESP_ERROR_CHECK(adc.begin());

    const BaseType_t task_created =
        xTaskCreate(weight_test_task, "weight_test_task", 4096, nullptr, 5,
                    &weight_test_task_handle);
    configASSERT(task_created == pdPASS);

    while (true) {
        xQueueReceive(q_protocol_to_main, &main_reg_act, portMAX_DELAY);
        show_reg_act(main_reg_act);
        reg_action(main_reg_act);
        show_reg_act(main_reg_act);
        xQueueSend(q_main_to_protocol, &main_reg_act, portMAX_DELAY);
    }
}
