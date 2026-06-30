/*
load cell reading task - read every 100ms no average 
communication taks - get command from client, serve it, send response to client - use queue to communicate with main task
commands 
- start measuring weight / volume
  if weight: set tare, measure to incremental weight or timeout,save time diff 
	if volume  weight for lower sensor, keep time , weight for upper sensor keep time save time diff 
-  measured time 
- measure slop (flow) variance  for weight measurement only 
- set weight increase 
- actuator # 
- actuation direction
- act command 
- measuring method - volume or weight 

*/

#include <stdio.h>
#include <cstdio>																												// (?) need both?
#include <string.h>
#include <esp_timer.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"


#include "asd1231.h"
#include "KeepCfg.h"
#include "NetServer.h"


static const char *SER_MAIN = "SER_MAIN";                               // Tag is a pointer to a string constant unify with others (?)
static const char *TAG = "MAIN";																				// unify (?)

const adc_channel_t agpio[4] = { ADC_CHANNEL_0, ADC_CHANNEL_1, ADC_CHANNEL_2, ADC_CHANNEL_3};  //delete (?)

// static SemaphoreHandle_t adc_chans_mutex;														// modify for the required mutex

QueueHandle_t q_protocol_to_main;																				
QueueHandle_t q_main_to_protocol;
static ADS1231 adc(GPIO_NUM_17, GPIO_NUM_16);														// why static (?) 
KeepCfg kc;																															
ProgramParameters new_params(0,4,0,0);                                	// new configuration parameter - define required pramaters (?)
ProgramParameters work_params(0,1800,0,0);                            	// working configuraiotn paramters 
struct regs_action main_reg_act;
LogRecord lr;																														// delete (?)	

void show_reg_act(regs_action areg_act){
	// show register states  after client command recevied - revise for new register structure
	int i;
	printf("function = %d, address: = %d\n", areg_act.func,areg_act.address);	 // decoded required function and address
	printf(" -- config register:"); 
	for (i = 0 ; i < 4 ; i ++) printf("%04x,", areg_act.config[i]);				// 0: configuration 
	printf("\n"); 
	printf(" -- clock register:"); 
	for (i = 0 ; i < 4 ; i ++) printf("%04x, ", areg_act.clock[i]);				// 1: clock
	printf("\n");     
	printf(" -- seq register:"); 
	for (i = 0 ; i < 4 ; i ++) printf("%04x,", areg_act.seq[i]);					// 2: log sequential number
	printf("\n");     
	printf(" -- record pointer register:"); 															// 3: record pointer - read only by client 
	printf("%04x,", areg_act.rec_point[0]);
	printf("\n");     
	printf(" -- log rec register:"); 																			// log record - read only by client (!)
	for (i = 0 ; i < 8 ; i ++) printf("%04x,", areg_act.log_rec[i]);
	printf("\n");     
}

// --------------------------------------------  load cell reading task  ---------------------------------------------

static void ads1231_task(void *arg){
	// read and display laod cell reading
	// exceute volume or weight measreuemt algorthim here 
	// based  measureing method value (read with sempahore ) measure time 
	// calculate slop of last 8 samples slop variance over the whole measurement
	// compare predicted value to threshold 
	// raise error if slope variance is too high
	// save measureed time and  slop variacne in registers 
  ADS1231::Sample sample;																										
	while (true) {
			if (adc.poll(sample)) {
					ESP_LOGI(TAG,
										"ADS1231 raw = %ld, normalized = %.7f",
										static_cast<long>(sample.raw),
										sample.normalized);
			} 
			else {
					// At 10 SPS and a 100 ms task period, sometimes you may call
					// slightly before DRDY goes low. This is normal.
					ESP_LOGD(TAG, "ADS1231 not ready");
			}
			vTaskDelay(pdMS_TO_TICKS(100));
	}
}

// -------------------------------------------------  server response to client -----------------------------------------

void reg_action(regs_action& areg_act){																	
	int i;

	// show register states  after client command received

	/
	int action_code = 10 * areg_act.func + areg_act.address;
	printf("action code = %d \n",action_code);
	switch(action_code){
			case 40:																														// measure weight or volume 
			areg_act.ret_code = 0;			
		break;

		case 41:														 																// write measured time to time_2_target register (2 words) - from load cell task (mutex)
			areg_act.ret_code = 0;			
		break;

		case 42:                                                         		// get last measurement variance , make sure client reads it only when measermement is completed 
			areg_act.ret_code = 0;			
		break;

		case 63:    																												// set weight measuement target - from register to load cels task (mutex)
			areg_act.ret_code = 0;
		break;

		case 46:                                                        		// actuate using actuator and dir 
			areg_act.ret_code = 0;	
		break;
	}
}

// ----------------------------------------------------- main ---------------------------------------------------------

extern "C" void app_main(void){ 

  //  load configuration parameters

  vTaskDelay(pdMS_TO_TICKS(2000));
  ESP_ERROR_CHECK(nvs_flash_init());
  kc.init(work_params);
  kc.load_parameters(work_params);                                        
  ESP_LOGI(SER_MAIN,",boot log_switch=%u",work_params.log_switch);       // show boot paramteres
  ESP_LOGI(SER_MAIN,",boot log_period=%u",work_params.log_period);
  vTaskDelay(pdMS_TO_TICKS(500));                                       

  //  create semaphores for mutual resources 

  // adc_chans_mutex = xSemaphoreCreateMutex();                         // modify for the required mutex 
	
	// esp_log_level_set("*", ESP_LOG_WARN);                              // Only warnings & errors
	q_protocol_to_main = xQueueCreate(10, sizeof(regs_action));
	q_main_to_protocol = xQueueCreate(10, sizeof(regs_action));
	static NetServer ns(q_main_to_protocol,q_protocol_to_main);

	vTaskDelay(pdMS_TO_TICKS(2000));
	// init gpio 
	nvs_flash_init();
	esp_netif_init();
	esp_event_loop_create_default();
	ns.wifi_init_ap();
	ns.start_server_task();
	ns.start_protocol_task();
  esp_err_t err = adc.begin();																					// error handling in begin for all (?)
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "ADS1231 init failed: %s", esp_err_to_name(err));
    return;
  }
  xTaskCreate(ads1231_task, "ads1231_task", 4096, nullptr, 5, nullptr); // diff between NULL and nulptr (?)


	while(true){
		xQueueReceive(q_protocol_to_main, &main_reg_act, portMAX_DELAY);
		printf(" -- reg act  to main: ------------------------------\n");
		show_reg_act(main_reg_act);
		reg_action(main_reg_act);																						//  command  received from client, serve it 
		printf(" -- reg act from main: -----------------------------\n");
		show_reg_act(main_reg_act);
		xQueueSend(q_main_to_protocol, &main_reg_act, portMAX_DELAY);
	}
}