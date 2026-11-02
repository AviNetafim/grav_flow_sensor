# ADC stability test 2

Tests ADS1231 readings alongside the ESP32 AP server, based on
`grav_flow_sensor_fw_2`. Weight mode only, run from the serial terminal without
Modbus client commands. `lib/ads1231` is an unchanged copy. `lib/NetServer` is a
copy with `wifi_off`, `wifi_on` and `wait_client_closed` added.

Edit the constants at the top of `src/main.cpp` before building:

| Constant | Default | Meaning |
| --- | --- | --- |
| `WIFI_ACTIVE` | `true` | `true`: AP and TCP server run. `false`: no Wi-Fi. |
| `WIFI_OFF_DURING_TEST` | `false` | `true`: Wi-Fi is on until `wt`, then off for the test. Needs `WIFI_ACTIVE = true`. |
| `ADC_READ_CRITICAL` | `true` | Read the ADC inside `portENTER_CRITICAL`. |
| `WEIGHT_TEST_CYCLE_MS` | `10` | ADC poll period. `100` matches the reference project. |
| `WEIGHT_TEST_MAX_SAMPLES` | `5000` | Samples array size. |
| `WEIGHT_TEST_STOP_SAMPLES` | `1000` | Temporary: test stops at this count, one sample per conversion. |
| `WEIGHT_TEST_USE_TARGET` | `false` | Temporary: `true` lets the target weight stop the test. |
| `WEIGHT_TEST_TIMEOUT_US` | `180000000` | Test stops after this time. |

Set `ADC_READ_CRITICAL = false` and `WEIGHT_TEST_CYCLE_MS = 100` to reproduce the
reference project's read timing.

## Terminal commands (115200 baud)

| Command | Action |
| --- | --- |
| `wt` | Start the weight test. Clears the previous samples. |
| `status` | Show test state, sample count and the options built in. |
| `print <index>` | Print up to 200 samples starting at `index`. Refused during a test. |
| `help` | List commands. |

`print` output is CSV: `index,time_ms,raw,weight_10mg`. The task reports progress
every 10 samples and the raw min, max and spread when it finishes.

`weight_10mg = CAL_OFFSET + raw * CAL_MUL / CAL_DIV`, with `CAL_MUL = 100` as in the
reference project, `CAL_OFFSET = -2828` and `CAL_DIV = 13908`.

The test stops at `WEIGHT_TEST_STOP_SAMPLES` (1000, about 100 s) or after
`WEIGHT_TEST_TIMEOUT_US` (180 s). The target weight is stored but not used while
`WEIGHT_TEST_USE_TARGET` is `false`. When `true`, the test also stops when the weight
has risen by the target since the first sample, and target 0 disables that stop.

## Python client

`adc_client.py` runs on Linux and Windows 11 with the standard library only. Each
command connects, does its work and disconnects. Addresses and timeouts are
constants at the top of the script.

| Command | Action |
| --- | --- |
| `python adc_client.py set-target <grams>` | Write the target weight register. |
| `python adc_client.py start` | Write the test register once, without retry. No acknowledgment means the server is assumed disconnected. |
| `python adc_client.py wait` | Try to connect every 4 s, showing elapsed time, until connected or `WEIGHT_TEST_TIMEOUT_S` (200 s). Then show `weight_stat` and disconnect. |
| `python adc_client.py get-samples <file>` | Save the samples of the completed test to a CSV file. An existing file is overwritten. |
| `python adc_client.py wifi-release` | Windows, administrator prompt: turn Wi-Fi automatic configuration on again. |

`get-samples` writes one `weight_10mg` value per line, with no header. Writing the
pointer register (address 6) makes the server copy 32 samples, starting at that index,
to the samples register (address 5). Each sample is one signed 16-bit word, limited to
-32768..32767. The pointer write answers code 1 during a test and code 2 if the
pointer is past the last sample.

The server stops Wi-Fi only after the client has received the start response and
disconnected, or after 2 s if it stays connected. Keep `WEIGHT_TEST_TIMEOUT_S` longer
than the server's test timeout. `wait` keeps trying if it connects before Wi-Fi has
stopped and reads state 1.

The PC cannot stay joined to `ESP32_AP` while the AP is off, and the OS may move to
another saved network. Each time `wait` fails to connect, it checks whether `ESP32_AP`
is visible. If it is and the PC is on another network, `wait` switches back with
`nmcli connection up` on Linux or `netsh wlan connect` on Windows. A saved profile
named `ESP32_AP` must exist. Windows may also need location services on for
`netsh wlan show networks` to work.

On Windows, `set-target`, `start` and `get-samples` first join `ESP32_AP` if needed.
Run the client from an administrator prompt to also keep the PC on it between commands.
The client then turns Wi-Fi automatic configuration off after `set-target` and after a
successful `wait`, and on again after `start`, `get-samples` and any failure. If you
stop after `set-target` or `wait`, run `wifi-release`, or the PC will not join networks
by itself. From a normal prompt the client leaves the setting alone and says that
Windows may move to another network between commands. `esp32_ap_connect.bat` is not
needed with this client.

The test assumes the ADS1231 runs at 10 samples per second, since it stores every
conversion. `time_ms` shows the real interval.

## Wi-Fi load

With no client connected the AP only sends beacons. To load the radio, connect to
`ESP32_AP` (password `12345678`) and run `ping 192.168.4.1`. Unsupported TCP commands
on port 502 are answered with error code 3.

With `WIFI_OFF_DURING_TEST = true`, `wt` closes the client connection, stops the AP
and waits for the stop event before reading the ADC. The test aborts without reading
if Wi-Fi has not stopped after 5 s. When the test ends or aborts, the AP starts again
and the client has to reconnect.

Build: `pio run`

Upload: `pio run -t upload`

Monitor: `pio device monitor`
