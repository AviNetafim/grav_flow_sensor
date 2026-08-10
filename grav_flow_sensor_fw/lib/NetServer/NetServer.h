/* class handling net with write to multiple register values in segments
 *  
* command structure
* general    |             header                                                 |         cmd payload         | CRC                | total length
* write:     |RTU | CMD | address | index low | index high | size low | size high | size X (val_low + val_high) | CRC low | CRC high |9 + size X 2
* read:      |RTU | CMD | address | index low | index high | size low | size high |                             | CRC low | CRC high |9
*
* response  structure
* general    |                     header                                         |   resp payload                            |  CRC               | total length
* write:     |RTU | CMD | address | index low | index high | size low | size high | compeltion code                           | CRC low | CRC high |10
* read:      |RTU | CMD | address | index low | index high | size low | size high | completion code | size X low_val+high_val | CRC low | CRC high |10 + size X 2
*/


#ifndef NETSERVER_h
#define NETSERVER_h

#include "esp_wifi.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include "esp_rom_crc.h"
#include "esp_log.h"
#include <string>
#include "freertos/queue.h"

#define TCP_PORT 502
#define MAX_BUF   128
#define COM_PRT False

static constexpr uint8_t REG_MAP_SIZE = 9;                                         // number of registers                 
static constexpr uint8_t REG_SAMPLES = 32;                                             // weight samples size 

#define myRTU 0x01                                                      // RTU ID
#define RTU 0                                                           // command index of RTU                                              
#define FUN 1                                                           // command index of command write (6) or read (4) or discover (2)                                            
#define ADD 2                                                           // command index of register number                                              
#define IXL 3                                                           // command index of register start element low byte                                            
#define IXH 4                                                           // command index of register start element high byte                                                                                        
#define SZL 5                                                           // command index of number of register # of elements low byte                                              
#define SZH 6                                                           // command index of number of register # of elements high byte                                                                                            
#define CODE 7                                                          // command index of completion code  

typedef struct {                                                        // register list metadata structure
  int dim;                                                              // register array size
  uint16_t *point;                                                      // pointer to 1st register array element
  }registers;

struct regs_action {
    uint8_t func;
    uint8_t address;
    uint16_t mode[1] = {0x01};                                          // measurement mode : weight or volume
    uint16_t target_weight[1] = {0x02};                                 // in 10mg units 
    uint16_t test[1] = {0x03};                                          // start test
    uint16_t vol_stat[2] = {0x04,0x05};                                 // volume test state
    uint16_t weight_stat[3] = {0x06,0x07,0x08};                         // volume and weight measure test time 
    uint16_t samples[REG_SAMPLES] = {0};                                // weight samples buffer 
    uint16_t pointer[1] = {0x09};                                       // pointer to samlpes buffer
    uint16_t actuator[2] = {0x0a,0x0b};                                 // actuator direction selection                     
    uint16_t actuate[1] = {0x14};                                       // actuate selected actuator and dier 
    uint8_t  ret_code;
};

class NetServer{
    public:
        NetServer (QueueHandle_t rxQueue, QueueHandle_t txQueue);
        void wifi_init_ap();
        void start_server_task();
        void start_protocol_task();
    private:
        static void server_task_entry(void *arg);                       // wrapper for tcp server task
        void tcp_server_task();
        static void protocol_task_entry(void *arg);                     // wrapper for protocol task
        void protocol_task();
        int parse_cmd();                                                // protocol implementation 
        uint16_t crc16_ccitt_false(const uint8_t *data, size_t len);    
        int resp_crc(const char* when, int arg_resp_size);              // crc calculation of protocol response
        void prt_msg(const char* when, uint8_t arg_msg[], int arg_msg_size);   // print message to serial monitor
        TaskHandle_t _protocol_task_handle;                             // used for handshaking between server and protocol taskd             
        QueueHandle_t _q_from_main;                                     // queues handles, created and pass to class by main
        QueueHandle_t _q_to_main;        
        int _client_sock = -1;
        uint8_t _rx_buf[MAX_BUF];                                       // tcp transmit buffer    
        int _rx_len = 0;
        uint8_t _tx_buf[MAX_BUF];                                       // tcp transmit buffer    
        int _tx_len;                                                    // transmit buffer length
        int _index;                                                     // command register index        
        uint8_t _payload_size;                                          // received command payload size  
        struct regs_action _reg_act;
        registers _regs[REG_MAP_SIZE] = {                               // register list metadata for protocol processing 
            {1,&_reg_act.mode[0]},
            {1,&_reg_act.target_weight[0]},
            {1,&_reg_act.test[0]},            
            {2,&_reg_act.vol_stat[0]},
            {3,&_reg_act.weight_stat[0]},
            {REG_SAMPLES,&_reg_act.samples[0]},
            {1,&_reg_act.pointer[0]},
            {2,&_reg_act.actuator[0]},
            {1,&_reg_act.actuate[0]}            
        };
};

#endif
