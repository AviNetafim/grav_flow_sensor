# ESP32-C6 + ADS1231 load-cell reader

Connections:

| ADS1231 | ESP32-C6 |
|---|---|
| DOUT | GPIO16 |
| SCLK | GPIO17 |
| GND | GND |
| Supply | A supply voltage supported by your ADS1231 board |

The application tares the empty load cell at startup, averages five conversions,
and prints a reading every three seconds at 115200 baud.

## Build, upload, and monitor

```sh
pio run
pio run -t upload
pio device monitor
```

## Calibrate weight

Initially, `COUNTS_PER_GRAM` in `src/main.cpp` is `1.0`, so the displayed weight
is an uncalibrated count difference. Note the raw value after startup tare, add
a known weight, and calculate:

```text
counts_per_gram = (loaded_raw - tare_raw) / known_weight_in_grams
```

Put that value (including a minus sign if needed) into `COUNTS_PER_GRAM`, then
rebuild and upload.
