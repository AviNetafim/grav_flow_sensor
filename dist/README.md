gravimetric flow sensor application user guide - ver 1.3

New in this version 
 updated configuration file:
  added parameters:
  "vol_timeout_low":, 200,  time out to reach low optical sensor 
  "vol_timeout_high": 600,  time out to reach high optical sensor 
  "weight_timeout": 900, time out to reach weight target
 min_flow parameter removed
 weight samples array increased to 5000 (measure time of 500s)
 linear regresion slop constantly calculated ( > min weight -0.5g>) last value is displayed in test result frame
latest update:
 shielded cable installed between the load cell to the controller board 
During the installation teste bench load cell was damaged and replaced by development load cell, calibration data in Par 5
1. general setup
 load cell and optical sensor reading and test processes are handled by a conreoller (server) in wifi access point mode.
 PC application (client) connects to server local network: "ESP32_AP"
 test step:
  power up controller 
  1st session process:
   look for and connect to "ESP_AP" network, password "12345678"
   navigate with file explorer to "grav_flow_sensor"
   right click in directory and select "open in terminal" to open a powershell window
   run application:  .\gfs.exe
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

4. configuration data 
 When program starts, configuration file is loaded and downloaded to controller
 Use load configuration button to reload modified configuration.

 configuration file:
  {
   "volume_0": 3,
   "volume": 12,
   "samples_file": "weight_samples",
   "vol_timeout_low": 200,
   "vol_timeout_high": 600,
   "weight_timeout": 900,
   "stable_time": 100,
   "cal_div": 14171,
   "cal_offset": -1970,
   "com_prt": 0,
   "show_sample": 0,
   "show_regs": 0
  }

5. load cell calibration:
 put a range of known loads in weight vessel 
 note load cell raw reading in calibrate mode for each load 
 Calculate linear regression
 update configuration calibration parameters 
  cal_offset = Intercept 
  cal_div = 1/(X Variable 1)* 100
 development load cell calibration:
  cal_offset = 381
  cal_div = 14166
 test bench load cell calibration:
  cal_offset = -4265
  cal_div = 12061
 development load cell calibration, installed on test bench:
  cal_offset = -1970
  cal_div = 14171
