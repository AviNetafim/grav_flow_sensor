
#include <stdio.h>
#include <cstdio>																												// (?) need both?
#include <string.h>
#include <esp_timer.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"


#include "ads1231.h"
#include "KeepCfg.h"
#include "NetServer.h"


static const char *SER_MAIN = "SER_MAIN";                               // Tag is a pointer to a string constant unify with others (?)
static const char *TAG = "MAIN";																				// unify (?)

// static SemaphoreHandle_t adc_chans_mutex;														// modify for the required mutex

QueueHandle_t q_protocol_to_main;																				
QueueHandle_t q_main_to_protocol;
static ADS1231 adc(GPIO_NUM_17, GPIO_NUM_16);														// why static (?) 
KeepCfg kc;																															
ProgramParameters new_params(0,4,0,0);                                	// new configuration parameter - define required pramaters (?)
ProgramParameters work_params(0,1800,0,0);                            	// working configuraiotn paramters 
struct regs_action main_reg_act;


void show_reg_act(regs_action areg_act){
	// show register states  after client command recevied - revise for new register structure
	int i;
	printf("function = %d, address: = %d \n", areg_act.func,areg_act.address);	 // decoded required function and address
	printf(" -- mode :"); printf("%04x \n", areg_act.mode[0]);
	printf(" -- target weight : %04x", areg_act.target_weight[0]); 
	printf(" -- test:"); printf("%04x \n", areg_act.test[0]);
	printf(" -- target register:");	printf("%04x,", areg_act.target_weight[0]);
	printf(" -- vol stat:"); printf("%04x \n", areg_act.vol_stat[0]);	
	printf(" -- samples:");
	for (i = 0 ; i < SAMPLES ; i ++)	printf(" %04x", areg_act.samples[i]);
	printf("\n");     
	printf(" -- actuaor:"); printf("%04x \n", areg_act.actuator[0]);
	printf(" -- direction:"); printf("%04x \n", areg_act.act_dir[0]);
	printf(" -- dactuate:"); printf("%04x \n", areg_act.actuate[0]);
}

// --------------------------------------------  load cell reading task  ---------------------------------------------

static void ads1231_task(void *arg){
	int loop_count = 0;
	int print_count = 0;
  ADS1231::Sample sample;																										
	while (true) {
		if (!adc.poll(sample)) {
			// At 10 SPS and a 100 ms task period, sometimes you may call
			// slightly before DRDY goes low. This is normal.
			ESP_LOGD(TAG, "ADS1231 not ready");			
		} 
		vTaskDelay(pdMS_TO_TICKS(100));
		if (loop_count++ > 30){
			print_count++;
			loop_count = 0;
				ESP_LOGI(TAG,
					"ADS1231 raw = %ld, normalized = %.7f",
					static_cast<long>(sample.raw),
					sample.normalized);
		} 
	} 
}

// -------------------------------------------------  server response to client -----------------------------------------

void reg_action(regs_action& areg_act){																	

	// show register states  after client command received

	// register map
	// 0 mode[1] 1 (weight) 0 (volume)
	// 1 target_weight[1] in 10mg units (up to 650g)  
	// 2 test[1] 1 to start a test , cleared by rtu 
	// 3 vol_stat[1] 0,1,2,3,4
	// 4 samples[10]
	// 5 test_time[1]  for both tests  im 0.1s  units 
	// 6 actuator[1]
	// 7 act_dir[1]
	// 8 actuate[1]
		
	// 	 set mode 
	//   set weight target 
	//   test 
	//   read samples 
	//   read volume stat 
	//   read test time 
	//   set actuator and dir 

	int action_code = 10 * areg_act.func + areg_act.address;
	printf("action code = %d \n",action_code);
	switch(action_code){

		case 60:																														//set test mode: volume or weight 
			printf("mode set to %d \n",areg_act.mode[0]);
			areg_act.ret_code = 0;			
		break;

		case 61:														 																// set weight target
			printf(" target weight is set to %d \n",areg_act.target_weight[0]);		
			areg_act.ret_code = 0;			
		break;

		case 62:                                                         		// start test
			printf("start a test \n");
			areg_act.test[0] = 0;		
			areg_act.ret_code = 0;			
		break;

		case 63:    																												// clear volume state regiser
			printf("clear volume test state \n");
			areg_act.vol_stat[0] = 0;		
			areg_act.ret_code = 0;			
		break;

		case 43:                                                        		// read volume state regiser
			printf("volume test state %d\n", areg_act.vol_stat[0]);
			areg_act.ret_code = 0;	
		break;

		case 44:                                                        		// read weight samples buffer
			printf("weight samples buffer :");
			for( int i = 0 ; i < SAMPLES ; i ++ ) printf(" %0x", areg_act.samples[i]);
			printf("\n");
			areg_act.ret_code = 0;	
		break;

		case 45:                                                        		// read test time 

			areg_act.ret_code = 0;	
		break;

	}
}

// ----------------------------------------------------- main ---------------------------------------------------------

extern "C" void app_main(void){ 

  //  load configuration parameters from flash 

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