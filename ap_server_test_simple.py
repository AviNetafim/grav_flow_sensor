# works with ap server
# use tcp com class 
# use cli to prepare aand send NetRtu command tp server 

from tcp_com import TcpCom
import sys
import json		


ESP32_IP = "192.168.4.1"
PORT = 502
SOCKET_TIMEOUT = 3

cfg_data = {
    "address": 0,
    "index": 0,
    "size": 0,
    "payload": [],
    "func" : 'r'
}

def load_config_data():
	try:
		with open('ap_server_tetst_cfg.json', 'r') as outfile:
			return json.load(outfile)
	except Exception as err:
		err_text = f"configuration file save error: {err=}, {type(err)=}"
		print(err_text)
		return -1

def save_config_data(arg_dict):

	json_object = json.dumps(arg_dict, indent=4)
	try:
		with open('ap_server_tetst_cfg.json', 'w') as outfile:
			outfile.write(json_object)
	except Exception as err:
		print(f"configuration file savr error: {err=}, {type(err)=}")
		return -1

def show_help():
	print(f"program options")
	print(f"  -help     	: show options")
	print(f"  -send     	: send command with set arguments")
	print(f"  -clrpl     	: clear payload")
	print(f"  -func rw  	: select function - read or write")	
	print(f"  -reg x    	: select register")
	print(f"  -index x  	: select register array index ")
	print(f"  -size x    	: select  function size ")
	print(f"  -pload x,y,z	: payload for write function ")


if __name__ == '__main__':
	temp_config_data=load_config_data() 
	for key in temp_config_data:
		cfg_data[key]= temp_config_data[key]
	print(f"configuration at start: {cfg_data}")

	tc= TcpCom(ESP32_IP,PORT,SOCKET_TIMEOUT)

	print(sys.argv)
	if len(sys.argv) == 2:
		if sys.argv[1] == '-help':
			show_help()
		elif sys.argv[1] == '-send':
			if cfg_data['func'] == 'r':
				error_code, rec_payload = tc.read(rtu=1, address= cfg_data['address'], index = cfg_data['index'], size=cfg_data['size'])
				if error_code == 0:
					print(rec_payload)
				else:
					print("error while reading from rtu")
			elif cfg_data['func'] == 'w':
				error_code = tc.write(rtu=1, address= cfg_data['address'], index = cfg_data['index'], size=cfg_data['size'], payload= cfg_data['payload'])
				if error_code > 0:
					print("error while weting to  rtu")
			else:
				print("wrong function")
		elif sys.argv[1] == '-clrpl':
			cfg_data['payload'].clear()
		else:
			print("2 wrong command")
	elif len(sys.argv) == 3:
		if sys.argv[1] == '-reg':
			cfg_data['address'] = int (sys.argv[2])
		elif sys.argv[1] == '-index': 
			cfg_data['index'] = int (sys.argv[2])
		elif sys.argv[1] == '-size': 
			cfg_data['size'] = int (sys.argv[2])
		elif sys.argv[1] == '-pload': 
			for i in sys.argv[2].split(','):
				cfg_data['payload'].append(int(i))
		elif sys.argv[1] == '-func': 
			cfg_data['func'] = sys.argv[2]
		else:
			print("wrong function parater")
			
	tc.sock.close()
	print(f"configuration at end: {cfg_data}")
	save_config_data(cfg_data)
	# 	else:
	# 		print ("wrong command")
	# else:
	# 	print("wrong number of arguments")    
	# if rw == 'r':
	# 	error_code, rec_payload = tc.read(rtu=1, address=t_address, index=t_index, size=t_size)
	# elif rw == 'w':
	# 	error_code, rec_payload = tc.write(rtu=1, address=t_address, index=t_index, size=t_size)
# error_code, rec_payload = tc.read(rtu=1, address=0, index=0, size=1)
# if error_code == 0:
# 	print(rec_payload)

# payload_data = [71,72,73]
# error_code = sp.write(rtu=1, address=0, index=8, size=3,payload = payload_data)
# if error_code > 0:
# 	print(error_code)
# error_code, rec_payload = sp.read(rtu=1, address=0, index=8, size=6)
# if error_code == 0:
# 	print(rec_payload)