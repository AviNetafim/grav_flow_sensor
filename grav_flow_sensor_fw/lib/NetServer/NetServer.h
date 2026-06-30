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

const uint8_t REG_MAP_SIZE = 8;

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
    uint16_t measure[1] = {0x01};                                       // start time 
    uint16_t time_2_target[2] = {0x02,0X03};                            // rtc data
    uint16_t variance [1] = {0x04};                                     // last numbers: last, pointed record
    uint16_t target[1] = {0x05};
    uint16_t actuator[1] = {0x06};                                      // full log record 
    uint16_t dir[1] = {0x07};                                           // full log record 
    uint16_t act[2] = {0x08};                                           // actuate                     
    uint16_t method[2] = {0x09};                                        // measruement method  weight = 0 volume = 1
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
            {1,&_reg_act.measure[0]},
            {2,&_reg_act.time_2_target[0]},
            {1,&_reg_act.variance[0]},            
            {1,&_reg_act.target[0]},
            {1,&_reg_act.actuator[0]},
            {1,&_reg_act.dir[0]},
            {1,&_reg_act.act[0]},
            {1,&_reg_act.method[0]}
        };
};

#endif
 
