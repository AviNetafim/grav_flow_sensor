# ADC stability test

This PlatformIO project provides a small TCP register interface for investigating
raw ADS1231 weight noise on the ESP32-C6. It reuses the firmware's NetServer and
ADS1231 drivers.

The device starts a Wi-Fi access point named `ESP32_AP` with password
`12345678` and listens for the register protocol on TCP port `502`.

## Raw weight test

Use the existing register protocol to configure and start the test:

- Register 9, element 7: sample period in milliseconds (`100`–`1000`, default
  `100`).
- Register 9, element 8: number of samples (`10`–`1000`, default `10`).
- Register 9, element 9: enable register dumps from `show_reg_act` (`1`).
- Register 0, element 0: select weight mode (`0`).
- Register 2, element 0: start the test (`1`, action `0x62`).

Each successful read is printed to the serial terminal as
`weight_test sample=<index>/<count> raw=<signed ADS1231 value>`.
Read weight status using action `0x44`: the four words report state, samples
completed, requested sample count, and period in milliseconds.

Build and upload with PlatformIO for the `esp32-c6-devkitm-1` environment. The
ADS1231 is connected to GPIO17 (SCLK), GPIO16 (DOUT), and GPIO18 (PWDN).
