#include "NetServer.h"

const char *TAG = "TCP";

NetServer::NetServer (QueueHandle_t rxQueue, QueueHandle_t txQueue){
  _q_from_main = rxQueue;
  _q_to_main = txQueue;
}

void NetServer::wifi_init_ap(){
    esp_netif_create_default_wifi_ap();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    wifi_config_t ap_config = {};

    strcpy((char*)ap_config.ap.ssid, "ESP32_AP");
    strcpy((char*)ap_config.ap.password, "12345678");

    ap_config.ap.ssid_len = strlen("ESP32_AP");
    ap_config.ap.max_connection = 1;
    ap_config.ap.authmode = WIFI_AUTH_WPA2_PSK;    

    esp_wifi_set_mode(WIFI_MODE_AP);
    esp_wifi_set_config(WIFI_IF_AP, &ap_config);
    esp_wifi_start();
    ESP_LOGI(TAG, "wifi initiated");
}

void NetServer::start_server_task(){
  xTaskCreate(server_task_entry, "Servertask", 4096, this, 5,NULL);  
}

void NetServer::server_task_entry(void *arg){
  NetServer *server = static_cast<NetServer*>(arg);   // Cast the parameter back to a NetServer pointer
  server->tcp_server_task();
  vTaskDelete(NULL);                                           
}  

void NetServer::tcp_server_task(){
    struct sockaddr_in server_addr;
    int listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(TCP_PORT);
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    ESP_LOGI(TAG, "tcp server task before binding");
    bind(listen_sock, (struct sockaddr *)&server_addr, sizeof(server_addr));
    ESP_LOGI(TAG, "tcp server task after binding");
    listen(listen_sock, 1);
    ESP_LOGI(TAG, "tcp server task after listen");
    while (1){
        ESP_LOGI(TAG, "Waiting for client");
        _client_sock = accept(listen_sock, NULL, NULL);
        ESP_LOGI(TAG, "Client connected");
        while (1){
            _rx_len = recv(_client_sock, _rx_buf, MAX_BUF, 0);
            if (_rx_len <= 0)
                break;
            /* Wake protocol task */
            xTaskNotifyGive(_protocol_task_handle);
        }

        close(_client_sock);
        _client_sock = -1;
        ESP_LOGI(TAG, "Client disconnected");
    }
}

void NetServer::start_protocol_task(){
  xTaskCreate(protocol_task_entry, "Servertask", 4096, this, 5,&_protocol_task_handle);  
}

void NetServer::protocol_task_entry(void *arg){
  NetServer *protocol = static_cast<NetServer*>(arg);   // Cast the parameter back to a NetServer pointer
  protocol->protocol_task();
  vTaskDelete(NULL);                                           
}  


//  Protocol Processing Task. Sleeps until TCP task wakes it.

void NetServer::protocol_task(){
  int retcode;
  while (1){
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);                            // client data is available
    if (_rx_len > 0) {                                                        
      if (COM_PRT){
         printf("Received %d bytes: ",_rx_len);
         for (size_t i = 0; i < _rx_len; i++)  printf("%02X ", _rx_buf[i]);
         printf("\n");
      }
      retcode = parse_cmd();
      if (COM_PRT) printf("parse ret code = %d\n",retcode);
      if (retcode < 0) {                                                // error in receive message           
          if(_tx_len > 0){                                              // send reponse to error
              send(_client_sock, _tx_buf, _tx_len, 0);            
          }
      }
      else{                                                             // receive message is ok, decode opcode
        _reg_act.func = _rx_buf[FUN];
        _reg_act.address = _rx_buf[ADD];
        xQueueSend(_q_to_main, &_reg_act, portMAX_DELAY);
        if (COM_PRT) printf("message sent to main\n");
        xQueueReceive(_q_from_main, &_reg_act, portMAX_DELAY);
        _tx_buf[CODE] = _reg_act.ret_code;
        if (_rx_buf[FUN] == 4) {
          for (int i = 0; i < _payload_size ; i++){                        
              _tx_buf[8+i*2] = (uint8_t)(*(_regs[_rx_buf[ADD]].point+_index+i) & 0xff );
              _tx_buf[9+i*2] = (uint8_t)((*(_regs[_rx_buf[ADD]].point+_index+i) >> 8) & 0xff);
          }
          _tx_len = resp_crc("read response: ",8 + _payload_size*2);
        }
        if ((_rx_buf[FUN] == 6)){
          _tx_len = resp_crc("write response: ",8);
        }
        if (_tx_len > 0){
            send(_client_sock, _tx_buf, _tx_len, 0);
        }
      }
    }    
  }
}

int NetServer::parse_cmd(){
  uint16_t CRC1 = 0;                                                  // for receive message crc calcualtion
  int i;
  _tx_len = 0;

  //  receive message checks, without response 

  prt_msg("cmd: ",_rx_buf,_rx_len);            
  if (_rx_buf[RTU] != myRTU){                                          // igonre command for other RTUs, no response
    printf("cmd is not for me");
    return -1;
  }
  
  if (_rx_len < 9){                                                    // ignore too short commands - no resposne
    prt_msg("incomplete command: ",_rx_buf,_rx_len);
    return -1;
  }

  CRC1 = crc16_ccitt_false(_rx_buf,_rx_len-2);
  if (((uint8_t)(CRC1 & 0XFF)) != _rx_buf[_rx_len-2] ||  (uint8_t)((CRC1) >> 8) != _rx_buf[_rx_len-1]){
    prt_msg("CRC error: ",_rx_buf,_rx_len);                             // CRC of received message is correct - no repsonse
    return -1;
  }

  //  receive message checks, with response 

  for (i = 0; i < 7 ; i++) _tx_buf[i] = _rx_buf[i];                      // copy command header to resp header
  if (_rx_buf[ADD] >= REG_MAP_SIZE){                                    // register address is not in RTU space send response with error cpde =1
    _tx_buf[CODE] = 0x01;
    _tx_len  = resp_crc("wrong register address: ",8);
    return -1;      
  }
  _payload_size = _rx_buf[SZL] + _rx_buf[SZH] * 256;
  _index =  _rx_buf[IXL] + _rx_buf[IXH] * 256;
  if (_index + _payload_size > _regs[_rx_buf[ADD]].dim){                   // command range exceeds register array size
    _tx_buf[CODE] = 0x02;
    _tx_len  = resp_crc("range too high: ",8);
    return -1 ;
  }

  if (_payload_size > (MAX_BUF - 10)/2){                                 // command payload is too big for buffers size
    _tx_buf[CODE] = 0x03;
    _tx_len  = resp_crc("payload to big: ",8);
    return -1;
  }

  //  process receives message by cmd type (r/w)

  switch(_rx_buf[FUN]){                                                 
    case 0x06:                                                          // write command
      if (_rx_len < 9 + _payload_size*2){                               // command is too short for palyload size  
        _tx_buf[CODE] = 0x04;
        _tx_len  = resp_crc("short payload: ",8);
        return -1;
      }  
    // valid write command 
      for ( i=0; i<_payload_size; i++){                                  // copy payload to register
        *(_regs[_rx_buf[ADD]].point+_index+i) = _rx_buf[7+2*i] + _rx_buf[8+2*i]*256;  // Little-endian    
      }

    //   _tx_buf[CODE]=0;                                               // response op code will be update after command processing
    //   _tx_len = resp_crc("write response: ",8);
      return 0;
    break;

    case 0x04:                                                          // read command
    //   for (i=0; i < payload_size ; i++){                             // read register range to repsonse message
    //     _tx_buf[8+i*2] = lowByte(*(_regs[_rx_buf[ADD]].point+_index+i));
    //     _tx_buf[9+i*2] = highByte(*(_regs[_rx_buf[ADD]].point+_index+i));
    //   }
    //   _tx_buf[CODE] = 0;
    //   _tx_len = resp_crc("read response: ",8 + payload_size*2);
        return 0; 
    break;
     
    default:
      _tx_buf[CODE] = 0x05;                                             // unrecoginzed command code
      _tx_len = resp_crc("wrong command: ",8);
      return -1;
    break;
  }                                                      
}

uint16_t NetServer::crc16_ccitt_false(const uint8_t *data, size_t len){
  uint16_t crc = 0xFFFF;
  while (len--) {
    crc ^= ((uint16_t)*data++) << 8;
    for (int i = 0; i < 8; i++) {
      if (crc & 0x8000)
        crc = (crc << 1) ^ 0x1021;
      else
        crc <<= 1;
    }
  }
  return crc;
}

int NetServer::resp_crc(const char* when,int arg_resp_size){
  uint16_t CRC2;
  CRC2 = crc16_ccitt_false(_tx_buf, arg_resp_size);
  _tx_buf[arg_resp_size]  = (uint8_t)(CRC2 & 0xff);
  _tx_buf[arg_resp_size+1] = (uint8_t)((CRC2 >> 8) & 0xff);
  prt_msg(when,_tx_buf,arg_resp_size+2);
  return (arg_resp_size+2);
}

void NetServer::prt_msg(const char* when, uint8_t arg_msg[], int arg_msg_size){
  if (COM_PRT){
    printf(when);
    for (int i = 0 ; i < arg_msg_size ; i++) printf("%02x,",arg_msg[i]);
    printf("\n");
  }
}
