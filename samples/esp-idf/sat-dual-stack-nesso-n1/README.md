# Hubble Network Satellite Dual-Stack Sample for the Arduino Nesso N1 on ESP-IDF

This sample runs **BLE and the Hubble Satellite Network** on an Arduino Nesso N1
and shows the radio state on its screen.

## Requirements

- A [Hubble device key](https://hubble.com/docs/guides/cloud-integration/register-devices).
- A [Hubble API token](https://hubble.com/docs/api-specification/hubble-platform-api#generate-an-api-key),
  for `dual-stack-companion.py` and [pyhubblenetwork](https://hubble.com/docs/guides/cli) commands.
- [ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/get-started/index.html) v6.1.
- An [Arduino Nesso N1](https://docs.arduino.cc/hardware/nesso-n1/) (ESP32-C6). The ESP32-C6
  transmits at [+20 dBm](../../../docs/integration_guides/common/prerequisites.rst) at the chip
  with no external amplifier. The gain of the Nesso N1's antenna is not characterized, so check
  the satellite link with an SDR.
- A computer with Bluetooth LE and internet access that meets the
  [pyhubblenetwork requirements](https://hubble.com/docs/guides/cli#requirements).
- [pyhubblenetwork](https://hubble.com/docs/guides/cli) (optional), for testing.
- The Hubble Connect app for
  [iOS](https://apps.apple.com/us/app/hubble-connect/id6751236191) or
  [Android](https://play.google.com/store/apps/details?id=com.hubble.connect)
  (optional), for the backend test.
- A software defined radio (SDR) for validation (suggested, but not required).

## Overview

This sample is [sat-dual-stack](../sat-dual-stack) with a user interface for the
Nesso N1's 1.14" 240 × 135 screen and front buttons. It is designed to:

- Demonstrate BLE and Hubble Satellite operation on a self-contained,
  battery-powered device.
- Show each stage of the dual-stack life-cycle on screen: provisioning, the
  countdown to the next pass, satellite transmission, and BLE beaconing.
- Show how to drive the Nesso N1's display, IO expanders, LED, battery
  charger and fuel gauge from ESP-IDF.

| Screen | Shown when |
| --- | --- |
| Loading | At boot. |
| Waiting for setup | The device advertises as **Hubble-ESP** until `dual-stack-companion.py` provisions it. |
| Next pass | Countdown to the next pass, with its local time (PT by default) and peak elevation. The ring is a 24-hour dial; the blurple rings mark the next three passes. |
| Transmitting | During a pass. The ring fills as packets are sent. The screen wakes 10 s before the pass, flashes blurple on each second of the last 10, and stays on until 10 s after the window. |
| Window complete | The transmission finished. The screen returns to the countdown. |
| Beaconing | Terrestrial mode: BLE beacons only, with an estimated count of packets sent today (UTC). |

The main screens show the battery level in the top-right corner, with a bolt
while the battery charges.

### Controls

The sample uses the two front buttons only; either one works. The touch panel
is put in hibernate at boot and never read.

| Action | Outside setup | In setup |
| --- | --- | --- |
| Click | Turn the screen off or on. | Next value. |
| Double-click | Open setup. | Change the value. |
| Hold 1 s | Nothing. | Save, close setup and turn the screen off. |

The radio keeps running with the screen off. The screen turns off after 10 s
idle. Setup closes after 30 s idle without saving, and any change goes back to
the saved value.

Setup shows one value at a time on the left, with the button legend on the
right.

| Value | Double-click |
| --- | --- |
| Next pass | Show the next of three passes. |
| Mode | Satellite or terrestrial. |
| Packets per pass | 16 (`HUBBLE_SAT_RELIABILITY_HIGH`, 10 s apart) or 8 (`HUBBLE_SAT_RELIABILITY_NORMAL`, 20 s apart). |
| Sat Tx power | Read only: 20 dBm at the chip. |
| Beacon every | Step through 0.5, 1, 1.5, 2, 5 and 10 s. Fixed while home Wi-Fi is in range. |
| Beacons today | Read only. |
| Battery | Read only: charge, current, voltage and temperature. |
| Payload | Read only. |
| Saved setup | Twice to clear the saved location and ephemeris and restart into setup. |

Mode, packets per pass and beacon interval are saved to flash on hold and
restored at boot. Clearing the saved setup keeps them.

> [!NOTE]
> All commands in this README are run from the sample folder containing this file:
> `<SDK path>/samples/esp-idf/sat-dual-stack-nesso-n1`

## Environment Setup

First, set up the environment. This step assumes you've installed esp-idf
to `~/esp/esp-idf`. If you haven't, follow the initial steps in the
[Installation guide](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/get-started/index.html#installation)
for your OS. Use the following command to export any necessary temporary environment variables for ESP-IDF to function.

### Windows

ESP-IDF does not support MSys, which is used by Git Bash. You must use PowerShell.

```ps1
& "C:\esp\<esp_idf_version>\esp-idf\export.ps1"
```

### Linux & macOS

```sh
source ~/esp/esp-idf/export.sh
```

### Python Dependencies

Then install the Python dependencies for the `dual-stack-companion.py`
provisioning script, and the [pyhubblenetwork](https://hubble.com/docs/guides/cli) tool for testing:

```sh
pip install -r ../../../tools/requirements-companion.txt pyhubblenetwork
```

### Set target chip

Set the target chip. This is needed once per build directory, and again after
editing `sdkconfig.defaults`.

```sh
idf.py set-target esp32c6
```

## BLE Advertising Parameters

The sample advertises with the following parameters by default. To change one,
edit the macro or option in the *Set by* column and rebuild. The beacon interval
can also be changed at runtime from the menu.

| Parameter | Value | Set by |
| --- | --- | --- |
| Beacon interval | 1364–1636 ms (1.5 s mean) | `ADV_INTERVAL_MIN_MS` / `ADV_INTERVAL_MAX_MS` in [main/app_ble.c](main/app_ble.c) |
| Provisioning interval (connectable) | 100–150 ms | NimBLE GAP fast interval `BLE_GAP_ADV_FAST_INTERVAL2_MIN` / `BLE_GAP_ADV_FAST_INTERVAL2_MAX` in [main/app_ble.c](main/app_ble.c) |
| Tx power | +3 dBm | `CONFIG_BT_LE_DFT_TX_POWER_LEVEL_P3` in [sdkconfig.defaults](sdkconfig.defaults) |

## Configurations

This sample exposes the following options (via `idf.py menuconfig`, under
"Sat Dual Stack Nesso N1 Sample Configuration"):

| Option | Default | Description |
| --- | --- | --- |
| `CONFIG_HUBBLE_DEVICE_KEY` | `""` | Hubble device cryptographic key, base64-encoded. |
| `CONFIG_HUBBLE_SAMPLE_DEBUG` | `n` | Debug mode: transmit 120 seconds after provisioning, then every 120 seconds, instead of at each predicted pass. |
| `CONFIG_HUBBLE_SAMPLE_WIFI_SSID` | `""` | 2.4 GHz Wi-Fi network for getting the time over SNTP at boot. Empty disables Wi-Fi. |
| `CONFIG_HUBBLE_SAMPLE_WIFI_PASSWORD` | `""` | Password for that network. |
| `CONFIG_HUBBLE_SAMPLE_SNTP_SERVER` | `"pool.ntp.org"` | SNTP server. |
| `CONFIG_HUBBLE_SAMPLE_TZ` | `"PST8PDT,M3.2.0,M11.1.0"` | Time zone for pass times, as a POSIX TZ string. |
| `CONFIG_HUBBLE_SAMPLE_TZ_LABEL` | `"PT"` | Label shown after pass times. |
| `CONFIG_HUBBLE_SAMPLE_HOME_SCAN_MINUTES` | `10` | How often to scan for the Wi-Fi network above. While it is in range the device is at home and beacons less often. 0 turns this off. |
| `CONFIG_HUBBLE_SAMPLE_HOME_BEACON_INTERVAL_MS` | `10000` | Beacon interval at home. |
| `CONFIG_HUBBLE_SAMPLE_SETUP_MAX_AGE_DAYS` | `0` | Days a saved setup is used before the device waits for the companion again. 0 keeps it until the companion runs again. |

## Build-time Provisioning

Set `CONFIG_HUBBLE_DEVICE_KEY` to your Hubble device key, in base64. The key
must be registered as **AES-256** with the **Unix time** counter source; see
[Register devices](https://hubble.com/docs/guides/cloud-integration/register-devices).
Set it in menuconfig:

```sh
idf.py menuconfig
```

or add the config `CONFIG_HUBBLE_DEVICE_KEY="your_b64_key"` to
`sdkconfig.defaults`.

> [!WARNING]
> `sdkconfig.defaults` is tracked by git, so a key added there is easy to commit
> by accident. `menuconfig` saves it to `sdkconfig` instead, which git ignores.

## Build and Flash

Build, flash, and open a serial logger. These steps can be done individually.
The Nesso N1 flashes through its native USB Serial/JTAG port; no buttons are
needed.

```sh
idf.py build flash monitor
```

If more than one board is connected, pick the port with `-p <PORT>`. Exit the
monitor with `Ctrl+]`.

## Runtime Provisioning

On boot, the device starts a connectable BLE advertisement named **"Hubble-ESP"**
and shows **Waiting for setup**. Use `dual-stack-companion.py` to push the
current Unix time, device location, and orbital parameters for the target
satellites.

See [companion tool documentation](../../../docs/satellite/companion-tool.rst)
for more information and instruction.

### Windows

```ps1
$env:HUBBLE_API_TOKEN = "<your-hubble-api-token>"

python ../../../tools/dual-stack-companion.py --label Hubble-ESP
```

### Linux & macOS

```sh
export HUBBLE_API_TOKEN=<your-hubble-api-token>

python ../../../tools/dual-stack-companion.py --label Hubble-ESP
```

### After Provisioning

A successful run ends with `Provisioned device successfully`, and the screen
changes to the next-pass countdown.

### Resuming After a Reset

The device saves the location and orbital parameters to NVS after each
provisioning. The Nesso N1 has no RTC, so the time is lost on every reset or
power cycle. If `CONFIG_HUBBLE_SAMPLE_WIFI_SSID` is set, the device joins that
network at boot, gets the time over SNTP, turns Wi-Fi off, and resumes from the
saved setup without the companion. The screen shows **Syncing time** meanwhile.

The device waits for the companion again when:

- No setup is saved, or **Saved setup** was cleared from the menu.
- Wi-Fi is not set, or joining or SNTP fails.
- `CONFIG_HUBBLE_SAMPLE_SETUP_MAX_AGE_DAYS` is not 0 and the saved setup is
  older than that.

> [!NOTE]
> The Wi-Fi password is stored in `sdkconfig`, which git ignores. Do not add it
> to `sdkconfig.defaults`.

## Program Flow

Once the firmware is flashed and the device has been provisioned:

1. In satellite mode, the device computes the next satellite pass and starts a
   timer for it.
2. Until the pass, the device advertises as a BLE beacon and shows the
   countdown.
3. At pass time, the timer fires. The device stops BLE advertising and
   transmits to the satellite, then shows **Window complete**.
4. The loop repeats for the next pass.

In terrestrial mode, the device only advertises as a BLE beacon. Switching mode
from the screen cancels the pending pass or stops the beacon, and the loop
starts again in the new mode.

The beacon advertising payload refreshes periodically at 1 hour interval.

```text
                power on / reset
                       |
                       v
       +-------------------------------+
       |  Initialize board, UI and BLE |
       |  ("Loading")                  |
       +-------------------------------+
                       |
                       v
       +-------------------------------------------+
       |  Connectable advertising "Hubble-ESP"     |
       |  ("Waiting for setup")                    |
       |                                           |
       |  dual-stack-companion.py writes orbital   |
       |  params, location + time over GATT        |
       +-------------------------------------------+
                       |  time set, peer disconnected
                       v
       +-------------------------------+
       |  Initialize the SDK           |
       |  (hubble_init +               |
       |  hubble_sat_satellites_set)   |
       +-------------------------------+
                       |
                       v
      ==================  MAIN LOOP  ==================
                       |
           +-----------+------------+
           | satellite              | terrestrial
           v                        v
  +---------------------+   +---------------------+
  |  Compute next pass  |   |  BLE beacon only    |
  |  ("Next pass")      |   |  ("Beaconing")      |
  +---------------------+   +---------------------+
           |                        |
           v                        | mode changed
  +---------------------+           |
  |  BLE beacon until   |           |
  |  the pass           |           |
  +---------------------+           |
           |                        |
           v                        |
  +---------------------+           |
  |  Stop BLE, send to  |           |
  |  satellite          |           |
  |  ("Transmitting")   |           |
  +---------------------+           |
           |                        |
           +-----------+------------+
                       |
                       +---> back to MAIN LOOP
```

## Test

### Terrestrial (BLE) Network

#### Local BLE Scanning

The `hubblenetwork ble scan` tool can be used to test the BLE beacon.
It scans for a device advertising with the given key and attempts to
decode its payload.

Pass the same key that was provisioned into the device, as base64:

```sh
hubblenetwork ble scan --key "<your base64 device key>"
```

Correctly decoded packets will stream to the console.

#### Backend Test

Download the Hubble Connect app to your smartphone and sign in, then keep the
phone near the device with the app open. The phone acts as a scanner: the
device appears in the app, and its packets appear on
[Hubble Dash](https://dash.hubble.com/dashboard).

### Satellite Network

To check the satellite transmission on the bench without waiting for a pass,
set `CONFIG_HUBBLE_SAMPLE_DEBUG=y` in menuconfig (see [Configurations](#configurations)).
The device then transmits 120 seconds after provisioning, and every 120 seconds after that.

Capture the packets with an SDR and decode them with your device key. See
[RF Verification with an SDR](../../../docs/integration_guides/common/next-steps.rst#rf-verification-with-an-sdr).

```sh
hubblenetwork sat scan --key "<your base64 device key>"
```

Correctly decoded packets will stream to the console. The 4-byte payload is the
battery temperature (int16, hundredths of a degree Celsius, big endian), the
battery percent, and a reserved byte.

Turn `CONFIG_HUBBLE_SAMPLE_DEBUG` back off to transmit at each predicted pass.

## Board Notes

| Peripheral | Connection |
| --- | --- |
| LCD | ST7789 on SPI2: SCLK 20, MOSI 21, DC 16, CS 17. Reset and backlight on expander 0x44 P1 and P6. |
| Touch | FT6336U at 0x38 on I2C: SDA 10, SCL 8. Hibernated at boot and not used. |
| Front buttons | Expander 0x43 P0 and P1, active low with pull-ups. Either button works. |
| LED | Expander 0x44 P7, active low. On during a satellite transmission. |
| Battery | BQ27220 fuel gauge at 0x55, AW32001 charger at 0x49. The sample enables charging and turns off the charger watchdog at boot, and logs the battery every minute. |
| LoRa | SX1262 shares the SPI bus; its CS (GPIO 23) is held high. |

The logo bitmap in `main/logo.c` is generated from the Hubble logomark:

```sh
python3 tools/gen_logo.py --width 24 > main/logo.c
```

## ESP-Mosaico (ESP32-S31)

The same sample also builds for the
[ESP-Mosaico](https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32s31/esp-mosaico/user_guide.html).
The IDF target picks the board: `esp32c6` builds the Nesso N1, and `esp32s31`
builds the ESP-Mosaico with `board_mosaico.c` and the touch UI in
`ui_mosaico.c`. The S31 has its own build directory and `sdkconfig`, so the
Nesso N1 configuration stays as it is.

> [!IMPORTANT]
> The S31 satellite radio calls `phy_set_freq_01k`, `phy_set_step_01k` and
> `phy_tx_tone`. The S31 PHY library in ESP-IDF v6.1 (esp-phy-lib `5695f4f`)
> does not have them, and the link fails. esp-phy-lib `20f1db05` or later
> has them. That update changes only the `esp32s31` libraries.

Build and flash:

```sh
idf.py -B build.esp32s31 -D SDKCONFIG=build.esp32s31/sdkconfig \
    -D IDF_TARGET=esp32s31 build
idf.py -B build.esp32s31 -D SDKCONFIG=build.esp32s31/sdkconfig \
    -p PORT flash monitor
```

The Type-C port is USB-OTG HS. The ESP32-S31 ROM has no console on it, so
`components/usb_hs_console` (from Espressif's esp-board-manager) starts a
TinyUSB CDC console before `app_main`. It also answers esptool's reset, so
`idf.py flash` works with no button. For the first flash, or if the
firmware does not start, hold BOOT (back of the CoreBoard) while you power
on the board.

### Controls

| Action | Result |
| --- | --- |
| Tap the dark screen, or press the front key | Wake the screen |
| Tap the marker at the top of the ring | Pick Satellite or Terrestrial |
| Swipe up, or tap the bar at the bottom | Open passes, transmission settings, payload and setup |
| Swipe down, or tap the sheet's handle | Close the sheet and save changed settings |
| Front key while the screen is on | Close the sheet, or turn the screen off |

The screen turns off after 30 s with no touch. It stays on through a
satellite window and for the 10 s before it.

### Board Notes

| Peripheral | Connection |
| --- | --- |
| Display | 2.16" 480x480 CO5300 AMOLED, QSPI on SPI2: CS 50, D0-D3 36/51/35/9. SCLK and reset swap between revisions. Power on GPIO 60, active low, soft-started with LEDC. |
| Touch | CST9220 at 0x5A, INT GPIO 6, no reset pin. |
| I2C | v1.0: SDA 0, SCL 1. v1.1 and v1.2: SDA 56, SCL 3. |
| Keys | Front (AI) GPIO 7, BOOT GPIO 61, active low. |
| LED | v1.0 only: GPIO 3, active low. On during a satellite transmission. |
| Vibration motor | GPIO 8. One short pulse when a satellite window starts. |
| Battery | BQ27220 fuel gauge at 0x55. The TP4057 charger has no I2C. |

The board revision comes from eFuse `USER_DATA`. If that is blank, the
sample probes for the fuel gauge on each revision's I2C pins.

Known limits:

- No 480x480 CO5300 init table is published. The driver's default table
  (for 466x466 modules) runs, and each draw sets its own window.
- Light sleep is off: USB-OTG stops in light sleep, and the console with it.
- The UI font is Manrope (SIL Open Font License, `main/fonts/OFL.txt`), cut
  to the glyphs the UI uses and drawn with LVGL's TinyTTF.
