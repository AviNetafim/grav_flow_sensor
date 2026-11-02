# ADC stability test 2

Tests ADS1231 readings alongside the ESP32 AP server, based on
`grav_flow_sensor_fw_2`. Weight mode only, run from the serial terminal without
Modbus client commands. `lib/ads1231` is an unchanged copy. `lib/NetServer` is a
copy with `wifi_off` and `wifi_on` added.

Edit the constants at the top of `src/main.cpp` before building:

| Constant | Default | Meaning |
| --- | --- | --- |
| `WIFI_ACTIVE` | `true` | `true`: AP and TCP server run. `false`: no Wi-Fi. |
| `WIFI_OFF_DURING_TEST` | `false` | `true`: Wi-Fi is on until `wt`, then off for the test. Needs `WIFI_ACTIVE = true`. |
| `ADC_READ_CRITICAL` | `true` | Read the ADC inside `portENTER_CRITICAL`. |
| `WEIGHT_TEST_CYCLE_MS` | `10` | ADC poll period. `100` matches the reference project. |
| `WEIGHT_TEST_NUM_SAMPLES` | `1000` | Samples per test, one per conversion. |

Set `ADC_READ_CRITICAL = false` and `WEIGHT_TEST_CYCLE_MS = 100` to reproduce the
reference project's read timing.

## Terminal commands (115200 baud)

| Command | Action |
| --- | --- |
| `wt` | Start the weight test. Clears the previous samples. |
| `status` | Show test state, sample count and the options built in. |
| `print <index>` | Print up to 200 samples starting at `index`. Refused during a test. |
| `help` | List commands. |

`print` output is CSV: `index,time_ms,raw,weight_10mg`. `weight_10mg` uses the
first sample as tare and the reference default divisor of 12060. The task reports
progress every 10 samples and the raw min, max and spread when it finishes.

The test assumes the ADS1231 runs at 10 samples per second, since it stores every
conversion. `time_ms` shows the real interval.

## Wi-Fi load

With no client connected the AP only sends beacons. To load the radio, connect to
`ESP32_AP` (password `12345678`) and run `ping 192.168.4.1`. TCP commands on port
502 are answered with error code 1.

With `WIFI_OFF_DURING_TEST = true`, `wt` closes the client connection, stops the AP
and waits for the stop event before reading the ADC. The test aborts without reading
if Wi-Fi has not stopped after 5 s. When the test ends or aborts, the AP starts again
and the client has to reconnect.

Build: `pio run`

Upload: `pio run -t upload`

Monitor: `pio device monitor`
