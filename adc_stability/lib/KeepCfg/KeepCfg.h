#ifndef KEEPCFG_H
#define KEEPCFG_H

#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"

#define DEFAULT_SWITCH 0
#define DEFAULT_PERIOD 1800

typedef struct{
    uint16_t log_switch;
    uint16_t log_period;
    uint16_t reserved_1;
    uint16_t reserved_2;    
} ProgramParameters;

class KeepCfg {
    public:
        void init(ProgramParameters &arg_params);
        void load_parameters(ProgramParameters &arg_params);
        void update_parameters(ProgramParameters &arg_new_params,ProgramParameters &arg_work_params); 
};

#endif