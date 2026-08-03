# works with grav_flow_sensor 
# use tcp com class 
# cli  board interface 
# created 30/06/2026

# register map
# 0 mode[1] 1 (weight) 0 (volume)
# 1 target_weight[1] in 10mg units (up to 650g)  
# 2 test[1] 1 to start a test , cleared by rtu 
# 3 vol_stat[1] 0,1,2,3,4
# 4 samples[10]
# 5 test_time[1]  for both tests  im 0.1s  units 
# 6 actuator[1]
# 7 act_dir[1]
# 8 actuate[1]

from tcp_com import TcpCom
import sys

ESP32_IP = "192.168.4.1"																								#  server access point address
PORT = 502
SOCKET_TIMEOUT = 3


def show_help():
	print(f"program options")
	print(f"  -help     		: show options")
	print(f"  -test   			: start a flow test, weight or volume")
	print(f"  -timeget 		  : get measure test time")
	print(f"  -volrst 		  : reset volume test state")
	print(f"  -act    			: update actuator state")
	print(f"  -mode w/v  		: set test mode")
	print(f"  -target ttt 	: set weight target to ttt")
	print(f"  -actuator 0/1	: set target actuator")
	print(f"  -dir 0/1    	: set target actruator state")


if __name__ == '__main__':

	tc= TcpCom(ESP32_IP,PORT,SOCKET_TIMEOUT)
	tc.print_com = False
	print(sys.argv)
	if len(sys.argv) == 2:
		if sys.argv[1] == '-help':
			show_help()

		elif sys.argv[1] == '-test':
			error_code = tc.write(rtu=1, address= 2, index = 0, size=1 , payload=[1])
			if error_code != 0:
				print(f"error while writing measure command to controller")
				exit()
		
		elif sys.argv[1] == '-timeget':
			error_code, time_2_target = tc.read(rtu=1, address= 5, index = 0, size=1)
			if error_code != 0:
				print(f"error while reading time to target from controller ")
				exit()
			print(time_2_target)

		elif sys.argv[1] == '-volrst':
			error_code = tc.write(rtu=1, address= 3, index = 0, size=1, payload=[0])
			if error_code != 0:
				print(f"error while reading time to target from controller ")
				exit()

		elif sys.argv[1] == '-act':
			error_code = tc.write(rtu=1, address= 8, index = 0, size=1, payload=[1])
			if error_code != 0:
				print(f" error while writing act command to controller ")
				exit()
		else:
			print("2 wrong command")
			exit()

	elif len(sys.argv) == 3:
		if sys.argv[1] == '-mode':
			sys.argv[2]
			if sys.argv[2] == 'w' or sys.argv[2] == 'v':
				mode = 'wv'.find(sys.argv[2])
				error_code = tc.write(rtu=1, address= 0, index = 0, size=1 , payload=[mode])
				if error_code != 0:
					print(f"error while writing weight target to controller")
				exit()			
			else:
				print(f" illegel mode value (w / v ")				

		elif sys.argv[1] == '-target':
			target = int(sys.argv[2])
			if target > 0 or target < 65000:
				error_code = tc.write(rtu=1, address= 1, index = 0, size=1, payload=[target])
				if error_code != 0:
					print(f"error while writing actuator number to controller")
					exit()
			else:
				print(f"illegal actuator number{target}")

		elif sys.argv[1] == '-actuator':
			actuator  = int(sys.argv[2])
			if actuator == 0 or actuator == 1:
				error_code = tc.write(rtu=1, address= 6, index = 0, size=1, payload=[actuator])
				if error_code != 0:
					print(f"error while writing actuator direction to controller")
					exit()
			else:
				print(f"illegal direction {actuator}")
				exit()

		elif sys.argv[1] == '-dir':
			direction  = int(sys.argv[2])
			if direction == 0 or direction == 1:
				error_code = tc.write(rtu=1, address= 7, index = 0, size=1, payload=[direction])
				if error_code != 0:
					print(f"error while writing actuator direction to controller")
					exit()
			else:
				print(f"illegal direction {direction}")
				exit()

		else:
			print("wrong function paratmer")
	else:
		print("1 wrong command")
	
	tc.sock.close()
