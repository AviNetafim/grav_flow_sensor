# grav_flow_sensor_fw
firmware based on bc_power_monitor 
measure flow by weight or volume 

main: init, wait for and repond  to user commands 
load cell reading task - read every 100ms ,stores data in a sample register with semaphore
communication task - get command from client, serve it, send response to client, use queue to communicate with main task 

user commands:
  set mode 
  set weight target 
  act 
  read samples 
  read volume stat 
  read test time 
  set actuator and dir 

rtu weight mode 
  start measring
  get tare value 
  measure time to tare + target
  store last 10 reading in  buffer
  when reading  >= tare + target: stop and save time (100ms increments) and number of samples

rtu volume mode
  start measuring 
  wait for lower sensor singal (to timeout)
  start recording time to upper sensor signal (to timeout)
  stop with upper sensor signal record elapsed time 

app weight mode:
  during test app reads samples register  and shows instat flow and elpased time 
  when target is reached show total time and calculated flow 
  
app volume mode:
  when lower sensor signal - vol stats rgister is set to 1 
  if lower timeout reached - vol stat register is set to 3
  when upper sensor signal - vol stat register is set to 2
  if upper timeout reached - vol stat register is set to 3
user clears vol stat rgister  before starting, read periodiclly during test an display status and time elapased

register map
0 mode[1] 1 (weight) 0 (volume)
1 target_weight[1] in 10mg units (up to 650g)  
2 test[1] 1 to start a test , cleared by rtu 
3 vol_stat[1] 0,1,2,3,4
4 samples[10]
5 test_time[1]  for both tests  im 0.1s  units 
6 actuator[1]
7 act_dir[1]
8 actuate[1]

volume test task 
start with notification as 

volume_test_task 
similar to weight_test_task.
starts in reg action case 62, ,case TestType::volume
waits for gpio20 negative edge stop at timeout_low
start measure time until negative edge og gpio19  or timeout_high
stop process when gpio19 negative edge is reached or timeout_low or timeout_high
test data strcutre
 - test_state (run/ stop)
 - time2tfill 
 - timeout_low
 - timeout_high

reg_action 43 read state and time2fill
 
pc app:
tkinter gui for  flow sensor client application 

main window title : gravimetric flow sensor test bench

radio buttons:  test method: weight / volume

entry fields:
  weight target,  float,  < 80g,   eneabled when test method == weight
  vessel volume,  float,  < 100cc  eneabled when test method == volume
  weight timeout, integer, < 1000 ds 
  volume timeout, integer, < 1000 ds 

display fields
  test time,  updated by run_test function if time value  < weight/volume timetout  otherwise  display error message
  flow  (test time/ weight target or vessle volume)
  error message 

buttons
start_test: run empty function run_test() 
get samples: run empty funvtion get samples()

delete unecceasry register action entries , code resets when defualt