# ADC stability firmware

Standalone copy of `grav_flow_sensor_fw_2` with a temporary weight-test task.

Edit `src/main.cpp` before building:

- `ADC_STABILITY_CYCLES_PER_SAMPLE`: 5–30 task cycles, default 10.
- `ADC_STABILITY_ITERATIONS`: 50–2000 recorded samples, default 500.
- `WEIGHT_TEST_SAMPLE_PERIOD_MS`: task period, inherited as 100 ms.

Start using the normal weight-mode test command. The task tares once at start,
polls the ADC every cycle, then prints raw ADC counts and integer `weight_10mg`
and appends that same weight to the sample buffer every configured number of
cycles. Each weight unit represents 10 mg. Conversion matches fw_2:
`(raw - tare_raw) * 100 / cal_div` (integer division).
A scheduled sample without a fresh ADC reading is retried on subsequent cycles.

The test stops after the configured number of recorded samples, regardless of
target weight or timeout. Defaults take approximately 500 seconds after tare.
The normal status register reports recorded sample count and elapsed deciseconds;
command `0x45` retrieves samples through the existing pointer/sample registers.
Flow statistics remain zero in this temporary test. Each new test clears the
buffer and counters. Existing calibration, networking, and volume mode remain
available.

Build: `pio run -d adc_stability`

Upload: `pio run -d adc_stability -t upload`

Monitor: `pio device monitor -d adc_stability`
