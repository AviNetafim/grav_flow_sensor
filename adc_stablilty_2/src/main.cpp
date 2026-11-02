#include <algorithm>
#include <cerrno>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "ads1231.h"
#include "NetServer.h"

static const char *TAG = "MAIN";

// Test options, edited by the programmer before building.
static constexpr bool WIFI_ACTIVE = false;                                  // true: AP + TCP server running, false: no wifi
static constexpr bool WIFI_OFF_DURING_TEST = false;                         // true: wifi stops for the weight test, needs WIFI_ACTIVE
static constexpr bool ADC_READ_CRITICAL = true;                             // true: read ADC inside portENTER_CRITICAL
static constexpr uint32_t WEIGHT_TEST_CYCLE_MS = 10;                        // ADC poll period, 100 = reference project behaviour
static constexpr size_t WEIGHT_TEST_NUM_SAMPLES = 1000;                     // samples per test, one every approx 100 ms

static constexpr size_t PRINT_MAX_SAMPLES = 200;                            // max samples printed by one print command
static constexpr size_t ACTIVITY_PRINT_SAMPLES = 10;                        // show task activity every 10 samples (approx 1 s)
static constexpr int64_t ADC_NOT_READY_TIMEOUT_US = 2000000;                // abort test if no conversion completes for 2 s
static constexpr uint32_t WIFI_SWITCH_TIMEOUT_MS = 5000;                    // abort test if wifi does not stop within 5 s
static constexpr bool WIFI_SWITCHED_BY_TEST = WIFI_ACTIVE && WIFI_OFF_DURING_TEST;
static constexpr int64_t CAL_MUL = 100;
static constexpr int64_t CAL_DIV = 12060;                                   // reference project default calibration divisor

enum class TestState : uint16_t {
    stop = 0,
    run = 1,
};

struct WeightSample {
    int32_t raw;                                                            // signed ADC reading
    uint32_t time_ms;                                                       // time since test started
};

struct WeightTestData {
    TestState test_state;
    size_t sample_count;
    WeightSample samples[WEIGHT_TEST_NUM_SAMPLES];
};

static ADS1231 adc(GPIO_NUM_17, GPIO_NUM_16, GPIO_NUM_18);
static portMUX_TYPE adc_spinlock = portMUX_INITIALIZER_UNLOCKED;
static SemaphoreHandle_t weight_test_mutex;
static SemaphoreHandle_t command_mutex;                                     // terminals run one command at a time
static TaskHandle_t weight_test_task_handle;
static WeightTestData weight_test_data = {TestState::stop, 0, {}};

static esp_err_t poll_adc(ADS1231::Sample &sample) {
    if (!ADC_READ_CRITICAL) {
        return adc.poll(sample);
    }
    portENTER_CRITICAL(&adc_spinlock);                                      // wifi interrupts cannot stretch the serial clock
    const esp_err_t err = adc.poll(sample);
    portEXIT_CRITICAL(&adc_spinlock);
    return err;
}

static void stop_weight_test() {
    xSemaphoreTake(weight_test_mutex, portMAX_DELAY);
    weight_test_data.test_state = TestState::stop;
    xSemaphoreGive(weight_test_mutex);
}

static void weight_test_task(void *arg) {
    NetServer *ns = static_cast<NetServer *>(arg);

    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);                            // wait for wt command

        const char *abort_reason = nullptr;
        if (WIFI_SWITCHED_BY_TEST) {
            ESP_LOGI(TAG, "Stopping wifi");
            const esp_err_t wifi_err = ns->wifi_off(pdMS_TO_TICKS(WIFI_SWITCH_TIMEOUT_MS));
            if (wifi_err != ESP_OK) {
                ESP_LOGE(TAG, "Wifi stop failed: %s", esp_err_to_name(wifi_err));
                abort_reason = "wifi did not stop";
            } else {
                ESP_LOGI(TAG, "Wifi stopped");
            }
        }

        ESP_LOGI(TAG, "Weight test started: samples=%u, poll cycle=%lu ms",
                 static_cast<unsigned>(WEIGHT_TEST_NUM_SAMPLES),
                 static_cast<unsigned long>(WEIGHT_TEST_CYCLE_MS));

        const int64_t started_us = esp_timer_get_time();
        int64_t last_ready_us = started_us;
        TickType_t next_wake = xTaskGetTickCount();
        bool stale_conversion = true;                                       // conversion already waiting at start has unknown age
        size_t count = 0;
        int32_t raw_min = INT32_MAX;
        int32_t raw_max = INT32_MIN;

        while (abort_reason == nullptr && count < WEIGHT_TEST_NUM_SAMPLES) {
            vTaskDelayUntil(&next_wake, pdMS_TO_TICKS(WEIGHT_TEST_CYCLE_MS));

            ADS1231::Sample adc_sample;
            const esp_err_t err = poll_adc(adc_sample);
            const int64_t now_us = esp_timer_get_time();
            if (err == ESP_ERR_NOT_FINISHED) {
                if (now_us - last_ready_us >= ADC_NOT_READY_TIMEOUT_US) {
                    abort_reason = "no conversion from ADC";
                    break;
                }
                continue;
            }
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "ADS1231 poll failed: %s", esp_err_to_name(err));
                abort_reason = "ADC error";
                break;
            }
            last_ready_us = now_us;
            if (stale_conversion) {
                stale_conversion = false;
                continue;
            }

            xSemaphoreTake(weight_test_mutex, portMAX_DELAY);
            weight_test_data.samples[count].raw = adc_sample.raw;
            weight_test_data.samples[count].time_ms =
                static_cast<uint32_t>((now_us - started_us) / 1000);
            weight_test_data.sample_count = ++count;
            xSemaphoreGive(weight_test_mutex);

            raw_min = std::min(raw_min, adc_sample.raw);
            raw_max = std::max(raw_max, adc_sample.raw);

            if (count % ACTIVITY_PRINT_SAMPLES == 0) {
                ESP_LOGI(TAG, "Weight test: sample %u/%u, raw=%ld",
                         static_cast<unsigned>(count),
                         static_cast<unsigned>(WEIGHT_TEST_NUM_SAMPLES),
                         static_cast<long>(adc_sample.raw));
            }
        }

        stop_weight_test();
        if (abort_reason != nullptr) {
            ESP_LOGE(TAG, "Weight test aborted: %s, samples=%u", abort_reason,
                     static_cast<unsigned>(count));
        } else {
            ESP_LOGI(TAG, "Weight test done: samples=%u, %" PRId64 " ms, raw min=%ld, max=%ld, spread=%ld",
                     static_cast<unsigned>(count),
                     (esp_timer_get_time() - started_us) / 1000,
                     static_cast<long>(raw_min), static_cast<long>(raw_max),
                     static_cast<long>(raw_max - raw_min));
        }
        if (WIFI_SWITCHED_BY_TEST) {
            const esp_err_t wifi_err = ns->wifi_on(pdMS_TO_TICKS(WIFI_SWITCH_TIMEOUT_MS));
            if (wifi_err != ESP_OK) {
                ESP_LOGE(TAG, "Wifi start failed: %s", esp_err_to_name(wifi_err));
            } else {
                ESP_LOGI(TAG, "Wifi started");
            }
        }
        ESP_LOGI(TAG, "Type print <index> to show samples");
    }
}

static void show_options() {
    printf("wifi=%s, adc critical section=%s, poll cycle=%lu ms, samples=%u\n",
           WIFI_SWITCHED_BY_TEST ? "off during test" : WIFI_ACTIVE ? "active" : "inactive",
           ADC_READ_CRITICAL ? "on" : "off",
           static_cast<unsigned long>(WEIGHT_TEST_CYCLE_MS),
           static_cast<unsigned>(WEIGHT_TEST_NUM_SAMPLES));
}

static void show_help() {
    printf("commands:\n");
    printf("  wt             start weight test\n");
    printf("  status         show test state and options\n");
    printf("  print <index>  print up to %u samples starting at index\n",
           static_cast<unsigned>(PRINT_MAX_SAMPLES));
    printf("  help           show this list\n");
}

static void read_test_state(TestState &state, size_t &count) {
    xSemaphoreTake(weight_test_mutex, portMAX_DELAY);
    state = weight_test_data.test_state;
    count = weight_test_data.sample_count;
    xSemaphoreGive(weight_test_mutex);
}

static void command_start() {
    xSemaphoreTake(weight_test_mutex, portMAX_DELAY);
    const bool running = weight_test_data.test_state == TestState::run;
    if (!running) {
        weight_test_data.test_state = TestState::run;
        weight_test_data.sample_count = 0;
    }
    xSemaphoreGive(weight_test_mutex);

    if (running) {
        printf("weight test already running\n");
    } else {
        xTaskNotifyGive(weight_test_task_handle);
    }
}

static void command_status() {
    TestState state;
    size_t count;
    read_test_state(state, count);
    printf("test %s, samples=%u/%u\n",
           state == TestState::run ? "running" : "stopped",
           static_cast<unsigned>(count),
           static_cast<unsigned>(WEIGHT_TEST_NUM_SAMPLES));
    show_options();
}

static void command_print(const char *arg) {
    char *end;
    const unsigned long index = strtoul(arg, &end, 10);
    while (*end == ' ') {
        ++end;
    }
    if (end == arg || *end != '\0') {
        printf("usage: print <index>\n");
        return;
    }

    TestState state;
    size_t count;
    read_test_state(state, count);
    if (state == TestState::run) {                                          // samples only print when the task is done
        printf("weight test running, %u samples so far\n", static_cast<unsigned>(count));
        return;
    }
    if (index >= count) {
        printf("index %lu out of range, samples=%u\n", index, static_cast<unsigned>(count));
        return;
    }

    const size_t last = std::min<size_t>(index + PRINT_MAX_SAMPLES, count);
    const int64_t tare_raw = weight_test_data.samples[0].raw;               // first sample tares, as in reference project
    printf("index,time_ms,raw,weight_10mg\n");
    for (size_t i = index; i < last; i++) {
        const WeightSample &sample = weight_test_data.samples[i];
        printf("%u,%lu,%ld,%ld\n", static_cast<unsigned>(i),
               static_cast<unsigned long>(sample.time_ms),
               static_cast<long>(sample.raw),
               static_cast<long>((sample.raw - tare_raw) * CAL_MUL / CAL_DIV));
    }
    if (last < count) {
        printf("next: print %u\n", static_cast<unsigned>(last));
    } else {
        printf("end of samples, total=%u\n", static_cast<unsigned>(count));
    }
}

static void run_command(const char *command) {
    xSemaphoreTake(command_mutex, portMAX_DELAY);
    if (strcmp(command, "wt") == 0) {
        command_start();
    } else if (strcmp(command, "status") == 0) {
        command_status();
    } else if (strcmp(command, "print") == 0) {
        printf("usage: print <index>\n");
    } else if (strncmp(command, "print ", 6) == 0) {
        command_print(command + 6);
    } else if (strcmp(command, "help") == 0) {
        show_help();
    } else {
        printf("unknown command, type help\n");
    }
    fflush(stdout);
    xSemaphoreGive(command_mutex);
}

static void terminal_task(void *arg) {
    const int input_fd = static_cast<int>(reinterpret_cast<intptr_t>(arg));
    char command[64];
    size_t length = 0;
    bool overflow = false;
    while (true) {
        unsigned char ch;
        if (read(input_fd, &ch, 1) != 1) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (ch == '\r' || ch == '\n') {
            if (length != 0 || overflow) {
                putchar('\n');
                fflush(stdout);
                command[length] = '\0';
                run_command(overflow ? "" : command);
            }
            length = 0;
            overflow = false;
        } else if (ch == '\b' || ch == 127) {
            if (length > 0) {
                --length;
                fputs("\b \b", stdout);
                fflush(stdout);
            }
        } else if (length < sizeof(command) - 1) {
            command[length++] = static_cast<char>(ch);
            putchar(ch);
            fflush(stdout);
        } else {
            overflow = true;
        }
    }
}

extern "C" void app_main(void)
{
    vTaskDelay(pdMS_TO_TICKS(2000));
    ESP_ERROR_CHECK(nvs_flash_init());

    weight_test_mutex = xSemaphoreCreateMutex();
    configASSERT(weight_test_mutex != nullptr);
    command_mutex = xSemaphoreCreateMutex();
    configASSERT(command_mutex != nullptr);

    QueueHandle_t q_protocol_to_main = xQueueCreate(10, sizeof(regs_action));
    QueueHandle_t q_main_to_protocol = xQueueCreate(10, sizeof(regs_action));
    configASSERT(q_protocol_to_main != nullptr);
    configASSERT(q_main_to_protocol != nullptr);
    static NetServer ns(q_main_to_protocol, q_protocol_to_main);

    if (WIFI_ACTIVE) {
        ESP_ERROR_CHECK(esp_netif_init());
        ESP_ERROR_CHECK(esp_event_loop_create_default());
        ns.wifi_init_ap();
        ns.start_server_task();
        ns.start_protocol_task();
    }

    const esp_err_t err = adc.begin();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ADS1231 init failed: %s", esp_err_to_name(err));
        return;
    }

    const BaseType_t task_created =
        xTaskCreate(weight_test_task, "weight_test_task", 4096, &ns, 5,
                    &weight_test_task_handle);
    configASSERT(task_created == pdPASS);

    show_options();
    show_help();
    fflush(stdout);

    const BaseType_t terminal_task_created =
        xTaskCreate(terminal_task, "terminal_task", 4096,
                    reinterpret_cast<void *>(static_cast<intptr_t>(fileno(stdin))), 5, nullptr);
    configASSERT(terminal_task_created == pdPASS);

#if CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG
    // The secondary console mirrors output, but stdin still reads only UART.
    // Read the USB VFS separately so either connector accepts commands.
    const int usb_input_fd = open("/dev/secondary", O_RDONLY | O_NONBLOCK);
    if (usb_input_fd < 0) {
        ESP_LOGE(TAG, "Could not open USB serial command input: %s", strerror(errno));
    } else {
        const BaseType_t usb_terminal_created =
            xTaskCreate(terminal_task, "usb_terminal", 4096,
                        reinterpret_cast<void *>(static_cast<intptr_t>(usb_input_fd)), 5, nullptr);
        configASSERT(usb_terminal_created == pdPASS);
    }
#endif

    // Test is run from the terminal, so TCP client commands are refused with
    // an error code. A connected client still generates wifi traffic.
    static regs_action main_reg_act;
    while (true) {
        xQueueReceive(q_protocol_to_main, &main_reg_act, portMAX_DELAY);
        main_reg_act.ret_code = 1;
        xQueueSend(q_main_to_protocol, &main_reg_act, portMAX_DELAY);
    }
}
