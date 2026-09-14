gravimetric flow sensor  application user guide
	
1. general setup
	load cell and optical sensor reading and test processes are handled by a conreoller (server) in wifi access point mode.
	PC apllication (client) connects to server local network: "ESP32_AP"
	test step:
		power up controller 
		1st time session process:
			look for and conncet to "ESP_AP" network, password "12345678"
			navigate with file explorer to "grav_flow_sensor"
			right click in directory and select "open in terminal" to open a powershell window
			run application : .\gfs.exe
			when session is terminated run wifi setup restoring: .\wifi_restore.bat
		on subsequent sessions:		
			navigate with file explorer to "grav_flow_sensor"
			right click in directory and select "open in terminal" to open a powershell window
			to connect to controller access point, run: .\esp32_ap_connect.bat
			run application: .\gfs.exe
			to restore wifi settings, run: .\wifi_restore.bat

	
2. test method selection
	Weight:
		When start test button is pressed, tare weight is read and load cell is samples every 100ms
		until target weight (in 10mg units) or timeout is reached.
	Volume:
		When start test button is pressed, low optical sensor is read every 25ms until stable high (configuralbe) or timeout 
		if low sesor atable high is reached, high sensor is read every 25m unitk stable high is reached or timeout
	calibrate:
		when start test button is pressed, load cell raw reading is displayed 
		

3. test setting:
		weight target: accumulated weight since weight test started for flow claculation 
		Flow range min: minimum test expcetd flow, used to calculate timeouts for volume and weight tests 

4. configuration data 
	when program start configuration file is loaded and downloaded to controller
	use load configuraton button to reload modified configuration.

	configuration file:
		{
			"volume_0": 3,						# water volume to low sensor in cc 
			"volume": 12,						# water volume between low nad high server 	
			"samples_file": "weight_samples",	# csv file name (w/o .csv extension) holding load cell readings 
			"stable_time": 100,					# optical sensors high stable time 		
			"cal_div": 0,						# calbiration factor - see par. 5
			"cal_offset: 0,						# calbiration offset - see par. 5 
			"com_prt": 0,						# print client / server communication on server and client terminals (debugging)
			"show_sample": 0,					# print weight test task load cell readings on server terminal every 1s (debugging)
			"show_regs": 0						# print server register map before and after any register action (debugging)
		}

5. load cell calbiration:
	put a range of known loads in weight vessel 
	note load cell raw reading in calibrate mode for each load 
	calculate linear regression
	update configuration calibration parameters 
		cal_offset = Intercept 
		cal_div = 1/(X Variable 1)* 100
	devevelopment load cell calibration:
		cal_offset = 381
		cal_div = 14166
	test bench load cell clibration:
		cal_offset = -4265
		cal_div = 12061
	devevelopment load cell calibration, installed on test bench:
		cal_offset = -1970
		cal_div = 14171
