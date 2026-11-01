#include "KeepCfg.h"

const char *TAGKCC = "KCC";

void KeepCfg::init(ProgramParameters &arg_params){
    // load pramaters, from nvs, create them if not present yet with default values
    nvs_handle_t nvs;
    int err;
    nvs_open("config", NVS_READWRITE, &nvs);                                // handle is tied to "config" namespace 
    err = nvs_get_u16(nvs,"log_switch",&arg_params.log_switch);             // read log switch from nvs
    ESP_LOGI(TAGKCC,",init:, get log_switch value err = %d",err);
    if (err == ESP_ERR_NVS_NOT_FOUND) {                                     // Key not present yet
        arg_params.log_switch = DEFAULT_SWITCH;                             // set default  value and save
        nvs_set_u16(nvs, "log_switch", arg_params.log_switch);             
        nvs_commit(nvs);        
    }
    ESP_LOGI(TAGKCC,",init:,nvs init log_switch value = %u",arg_params.log_switch);    

    err = nvs_get_u16(nvs,"log_period",&arg_params.log_period);             // same for log period    
    ESP_LOGI(TAGKCC,",init:, nvs init get log_period value err = %d",err);
    if (err == ESP_ERR_NVS_NOT_FOUND) {                                     
        arg_params.log_period = DEFAULT_PERIOD;
        nvs_set_u16(nvs, "log_period",arg_params.log_period );
        nvs_commit(nvs);
    }    
    ESP_LOGI(TAGKCC,",init:, nvs init log_period value = %u",arg_params.log_period);        
    nvs_close(nvs);
}

void KeepCfg::load_parameters(ProgramParameters &arg_params){
    nvs_handle_t nvs;
    int err;
    nvs_open("config", NVS_READWRITE, &nvs);                          
    err = nvs_get_u16(nvs,"log_switch",&arg_params.log_switch);
    if (err != 0){
        arg_params.log_switch = DEFAULT_SWITCH;
        ESP_LOGI(TAGKCC,",load parameters:, get log switch value err = %d",err);
    }  
    err = nvs_get_u16(nvs,"log_period",&arg_params.log_period);
    if (err != 0){
        arg_params.log_period = DEFAULT_PERIOD;
        ESP_LOGI(TAGKCC,",load parameters:, get log period value err = %d",err);        
    } 
    nvs_close(nvs);
}

void KeepCfg::update_parameters(ProgramParameters &arg_new_params,ProgramParameters &arg_work_params){
    nvs_handle_t nvs;
    nvs_open("config", NVS_READWRITE, &nvs);
    if (arg_work_params.log_switch != arg_new_params.log_switch){
        ESP_LOGI(TAGKCC,",update_parameters:, new log switch value %u, updating nvs",arg_new_params.log_switch);
        nvs_set_u16(nvs, "log_switch", arg_new_params.log_switch);
        arg_work_params.log_switch =  arg_new_params.log_switch;
    }
    else ESP_LOGI(TAGKCC,",update_parameters:, no change in log switch");
    if (arg_work_params.log_period != arg_new_params.log_period){
        ESP_LOGI(TAGKCC,",update_parameters:,new log period value %u, updating nvs",arg_new_params.log_period);
        nvs_set_u16(nvs, "log_period", arg_new_params.log_period);
        arg_work_params.log_period =  arg_new_params.log_period;
    }
    else ESP_LOGI(TAGKCC,",update_parameters:, no change in log period");
    nvs_commit(nvs);
    nvs_close(nvs);
}
