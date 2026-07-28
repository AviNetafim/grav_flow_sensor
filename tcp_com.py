# tcp NetRtu com class, including :
#   tcp get and send msg functions 
#   NetRtu read and write commands, 
import socket
from crccheck import crc

class TcpCom:
    def __init__(self, ip_add,port ,socket_timeout):
        try:
            self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            self.sock.settimeout(socket_timeout)
            self.sock.connect((ip_add, port))
            print("Connected to server")
        except Exception as e:
            print("Connection failed:", e)
        except ConnectionRefusedError:
            print("Server refused connection")
        except TimeoutError:
            print("Connect timeout")
        except OSError as e:
            print(f"OS error during connect: {e}")
        except Exception as e:  
            print(f"Unexpected error: {e}")
        self.rec_str =[]
        self.print_com = False

    def write(self,rtu=1, address=0,index=0,size=1, payload=[]):
        cmd_str = bytearray([rtu, 0x06, address])
        cmd_str.extend(bytearray([index % 256,index//256]))
        cmd_str.extend(bytearray([size % 256,size//256]))
        for i in range(size):
            cmd_str.extend(bytearray([payload[i] % 256,payload[i] //256 ]))
        crc_val = crc.Crc16CcittFalse.calc(cmd_str)
        cmd_str.extend(bytearray([crc_val % 256,crc_val//256]))
        retry = 0
        while retry < 3:
            if self.print_com:
                print('cmd str:' + ' '.join(f"{x:02x}" for x in cmd_str))
            error_code = 0       
            if self.send_msg(cmd_str) > 0 :
                retry += 1
                error_code = 0x10
            else:
                #  command sent successfully, read response
                if self.get_msg() > 0:
                    retry += 1
                    error_code = 0x20
                else:
                    # expected response length, parse response command
                    if self.print_com:
                        print('write received:' + ' '.join(f"{x:02x}" for x in self.rec_str))
                    error_code = error_code | self.rec_str[7] 
                    crc_val = crc.Crc16CcittFalse.calc(bytearray(self.rec_str[0:8]))
                    if self.rec_str[8] != crc_val %256  or self.rec_str[9] != crc_val//256:
                        error_code = error_code | 0x40 
                    if error_code != 0:
                        retry += 1
                    else:
                        break
            if error_code > 0:
                print(f"error {error_code:x} while writing {address} to rtu")
        return error_code

    def read(self,rtu=1, address=0, index=0, size=1):
        self.rec_str=[]
        cmd_str = bytearray([rtu, 0x04, address])
        cmd_str.extend(bytearray([index % 256,index//256]))
        cmd_str.extend(bytearray([size % 256,size//256]))
        crc_val = crc.Crc16CcittFalse.calc(cmd_str)
        cmd_str.extend(bytearray([crc_val % 256,crc_val//256]))
        payload = [] 
        retry = 0
        while retry < 3:
            if self.print_com:
                print('cmd str:' + ' '.join(f"{x:02x}" for x in cmd_str))
            error_code = 0
            if self.send_msg(cmd_str) > 0 :
                retry += 1
                error_code = 0x10 
            else:
                #  command sent successfully, read response
                if self.get_msg() > 0:
                    retry += 1
                    error_code = 0x20
                else:
                    # expected response length, parse response command  
                    if self.print_com:                  
                        print('read received:' + ' '.join(f"{x:02x}" for x in self.rec_str))                    
                    error_code = error_code | self.rec_str[7]
                    crc_val = crc.Crc16CcittFalse.calc(bytearray(self.rec_str[0:8+size*2]))
                    if self.rec_str[8+size*2] != crc_val %256  or self.rec_str[9+size*2] != crc_val//256:
                        error_code = 0x40  | self.rec_str[7]
                    if error_code != 0:
                        retry += 1
                    #  get here if no errors in command response 
                    else:
                        for i in range(size):
                            payload.append(self.rec_str [8 + 2*i] + 256*self.rec_str [9 + 2*i])
                        break 
        if error_code > 0:
            print(f"error {error_code:x} while reading {address} from rtu")
        return error_code, payload 
    
    def send_msg(self,arg_msg):
        """send message to server"""
        try:
            self.sock.sendall(arg_msg)
            return 0
        except BrokenPipeError:
            print("Server closed connection (broken pipe)")
            return 1
        except ConnectionResetError:
            print("Connection reset by server")
            return 2        
        except TimeoutError:
            print("Send timeout")
            return 3        
        except OSError as e:
            print(f"Send OS error: {e}")
            return 4
        except Exception as e:  
            print(f"Unexpected error: {e}")
            return 5

    def get_msg(self):
        try:
            self.rec_str = self.sock.recv(1024)
            if  self.rec_str == b'':
                print("Connection closed by peer")
                return 1
            # print('data:' + ' '.join(f"{x:02x}" for x in  self.rec_str))
            return 0
        except socket.timeout:
            print("Read timed out")
            return 2
        except BlockingIOError:
            print("No data available (non-blocking mode)")
            return 3
        except ConnectionResetError:
            print("Connection reset by peer")
            return 4
        except OSError as e:
            print(f"Other socket error: {e}")
            return 5
        except Exception as e:  
            print(f"Unexpected error: {e}")
            return 6


if __name__ == '__main__':
    pass