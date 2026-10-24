# Hubble Network Dual-Stack Sample on the Arduino Nesso N1

This sample runs **BLE and the Hubble Satellite Network** on an
[Arduino Nesso N1](https://docs.arduino.cc/hardware/nesso-n1/) (ESP32-C6), and
shows the radio state on its 1.14" 240 × 135 touchscreen.

It is the [sat-dual-stack](../sat-dual-stack) sample plus a display UI. The
radio behavior, provisioning and test steps are the same; see that README for
requirements, the device key, and `dual-stack-companion.py`.

## Screens

| Screen | Shown when |
| --- | --- |
| Loading | Boot. |
| Waiting for setup | Advertising as **Hubble-ESP** until `dual-stack-companion.py` provisions time, location and ephemeris. |
| Next pass (black) | Countdown to the next pass. The ring is a 24 h dial; the blurple rings are the next three passes. |
| Transmitting (blurple) | Satellite pass. The ring fills as packets go out (`HUBBLE_SAT_RELIABILITY_HIGH`, 16 packets, 10 s apart, plus drift retries). |
| Window complete (green) | The transmission finished. Returns to the countdown. |
| Beaconing | Terrestrial mode: BLE beacons only, with an estimated count of packets today (UTC). |

Tap the marker at the top of the ring to choose **Satellite** (satellite at
each pass, BLE beacon between passes) or **Terrestrial** (BLE beacon only).

## Front Button and Menu

- **Click**: close the menu if it is open; otherwise turn the screen off or
  on. The radio keeps running.
- **Double-click**: open or close the menu.

The menu shows one value at a time: the value on the left, **+** and **−** on
the right. Swipe up or down, or tap the left side, to move between values.

| Value | Editable |
| --- | --- |
| Mode | Satellite or terrestrial |
| Next pass | Browse the next three passes |
| Packets per pass | 16 (high, 10 s apart) or 8 (normal, 20 s apart) |
| Sat Tx power | No: 20 dBm at the chip; antenna gain not included |
| Beacon every | 0.5, 1.1, 2, 5 or 10 s |
| Beacons today | No |
| Battery | No |
| Payload | No |

Settings are held in RAM and reset on reboot, like the provisioning data.

The satellite payload is 4 bytes: battery temperature (int16, hundredths of a
degree Celsius, big endian), battery percent, and a reserved byte.

## Build and Flash

```sh
source ~/esp/esp-idf/export.sh
idf.py set-target esp32c6
idf.py menuconfig   # set CONFIG_HUBBLE_DEVICE_KEY
idf.py build flash monitor
```

The board flashes through its native USB Serial/JTAG port; no buttons are
needed.

## Board Notes

- LCD: ST7789 on SPI2 (SCLK 20, MOSI 21, DC 16, CS 17). Reset and backlight
  are on the PI4IOE5V6408 expander at 0x44 (P1, P6).
- Touch: FT6336U at 0x38 on I2C (SDA 10, SCL 8).
- LED: expander 0x44 P7, active low. It is on during a satellite transmission.
- Keys: expander 0x43 P0 and P1, active low with pull-ups. Either key works.
- The SX1262 LoRa radio shares the SPI bus; its CS (GPIO 23) is held high.
