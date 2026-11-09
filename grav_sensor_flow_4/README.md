# grav_sensor_flow_4

Revision of `grav_sensor_flow_3`. Same register protocol, tests and `flow_sensor_cfg.json`.

## Server

- The AP answers the Windows internet probes (`lib/NcsiResponder`, unchanged), so Windows
  sees `ESP32_AP` as having internet and stays on it.
- The esp32-c6-devkitm-1 RGB LED (GPIO8) shows the state, in green at half intensity
  (`STATUS_LED_GREEN = 128`):

| LED | State |
| --- | --- |
| on | Wi-Fi ready for a client |
| off, then blinking 0.5 s on / 0.5 s off | `cal`, `w2t`, `w2g`: start acknowledged, Wi-Fi off, test running |
| blinking | `vol`: test running, Wi-Fi stays on |
| on again | test done and Wi-Fi ready, results can be read |

Register changes from `grav_sensor_flow_3`:

| Address | Register | Change |
| --- | --- | --- |
| 4 | `weight_stat[5]` | Word 4 added: `time_ds`. `w2g`: tare sample to last sample (0 if no flow sensed). `w2t`, `cal`: first to last sample. |
| 8 | `last_test[2]` | New, read only: mode and target of the last started test. Mode `0xFFFF` before the first test. For `cal` the target is the test weight. |

## Client

`flow_client.py`, standard library only, Linux and Windows 11.

| Command | Action |
| --- | --- |
| `cal <test_weight>` | Start calibration, test weight in 10 mg units. |
| `w2t [samples_count]` | Start weight to time, 10-5000 samples. |
| `w2g [weight_increment]` | Start weight to increment, 500-4000 x 10 mg. |
| `vol` | Start volume test. |
| `get` | Show and save the results of the last test. |

`cal`, `w2t`, `w2g` and `vol` join the server's Wi-Fi, write the configuration and test
registers, and disconnect once the start is acknowledged, without waiting for the test.
The start is never sent twice. Run `get` when the LED is green again.

Joining waits up to `WIFI_READY_TIMEOUT_S` (60 s) for the server's Wi-Fi. On Windows it
does what `esp32_ap_connect.bat` does: `netsh wlan set autoconfig enabled=yes` (needs an
administrator prompt, skipped otherwise), `netsh wlan connect`, then shows the interface
state. On Linux it runs `nmcli connection up ESP32_AP`. Both need a saved Wi-Fi profile
named `ESP32_AP`.

`get` results by last test:

| Test | Shown | Saved |
| --- | --- | --- |
| `cal` | Status, test weight, average raw weight | Row `test_weight,raw_weight` appended to `calibration_file`.csv, not repeated if `get` runs twice |
| `w2t` | Status, sample count, time, average weight | All 10 mg readings to `samples_file`.csv |
| `w2g` | Status, time from tare to target weight | All 10 mg readings to `samples_file`.csv |
| `vol` | Status, time from low to high optical sensor | Nothing |

Build: `pio run`. Upload: `pio run -t upload`. Monitor: `pio device monitor`.
