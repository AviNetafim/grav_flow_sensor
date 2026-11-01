# ADC stability firmware

Standalone copy of `grav_flow_sensor_fw_2` with a temporary weight-test task.

Edit `src/main.cpp` before building:

- `ADC_STABILITY_CYCLES_PER_SAMPLE`: 5–30 task cycles, default 10.
- `ADC_STABILITY_ITERATIONS`: 50–2000 recorded samples, default 200.
- `WEIGHT_TEST_SAMPLE_PERIOD_MS`: task period, inherited as 100 ms.

Wi-Fi and the network server are disabled for this temporary test
(`ADC_STABILITY_WIFI_ENABLED = false`). Open the serial terminal at 115200 baud,
type `wt`, and press Enter to start (`weight_test_task` also works). Typed characters
are echoed by the firmware, including backspace editing. Disable local echo in
your terminal to avoid duplicate characters. Repeat the command after a test
finishes to run again; commands during a test do not restart it.
Commands are accepted through both the UART console and the secondary native USB
Serial/JTAG console. If `/dev/esp32c6` is unavailable, specify the connected port
with `pio device monitor -p /dev/ttyACM0` (or the actual UART/USB device path).

The task tares once at start,
polls the ADC every cycle, then prints raw ADC counts and integer `weight_10mg`
and appends that same weight to the sample buffer every configured number of
cycles. Each weight unit represents 10 mg. Conversion matches fw_2:
`(raw - tare_raw) * 100 / cal_div` (integer division).
A scheduled sample without a fresh ADC reading is retried on subsequent cycles.

The test stops after the configured number of recorded samples, regardless of
target weight or timeout. Defaults take approximately 200 seconds after tare.
The normal status register reports recorded sample count and elapsed deciseconds;
command `0x45` retrieves samples through the existing pointer/sample registers.
Flow statistics remain zero in this temporary test. Each new test clears the
buffer and counters. Network commands are unavailable while Wi-Fi is disabled.

Build from this directory: `pio run`

Upload: `pio run -t upload`

Monitor: `pio device monitor`
