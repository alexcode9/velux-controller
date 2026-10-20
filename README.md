# VELUX KLI controller

NodeMCU V3 (ESP8266) firmware that pulses two VELUX KLI remotes for Home Assistant. Each remote is an MQTT cover with Open, Close, and Stop. There is no position sensor: Home Assistant tracks those commands optimistically, and a press on the physical remote does not update Home Assistant.

The GPIOs are wired straight to the remote button pads. A press drives the pad low. A release leaves the pad high impedance. The firmware never drives a pad high.

## Wiring

Common ground between the NodeMCU and both remotes. One GPIO per button pad. No transistors, relays, or separate sense wires.

| Remote | Button | NodeMCU | GPIO |
| --- | --- | --- | --- |
| 1 | Up | D1 | GPIO5 |
| 1 | Stop | D2 | GPIO4 |
| 1 | Down | D5 | GPIO14 |
| 2 | Up | D6 | GPIO12 |
| 2 | Stop | D0 | GPIO16 |
| 2 | Down | D7 | GPIO13 |

Confirm electrical compatibility and active-low behaviour before connecting a remote. With the remote's own power applied and the NodeMCU ground connected, measure each pad while it is idle and while that remote's button is held:

- Idle must be a voltage the ESP8266 can tolerate on an input. A released GPIO is high impedance and will follow the pad.
- Held must be near 0 V (active low).

Do not connect a pad that idles above 3.3 V. GPIO0, GPIO2, GPIO15, TX, and RX are not used.

If a remote is powered from the NodeMCU 3.3 V pin, remove its batteries first and confirm that the remote accepts 3.3 V and that the NodeMCU regulator can supply it while the radio transmits. Leaving batteries in parallel with the 3.3 V pin can charge them or back-feed the board. Sharing ground and the button lines, with each remote on its own battery, is the other arrangement, and it still needs the voltage check above.

## How a press works

Every button, including GPIO16, uses the same sequence:

1. Write the output latch low while the pin is still an input.
2. Switch the pin to output. It then drives low. Setting the latch first avoids a high glitch when the driver turns on.
3. When the pulse ends (200 ms by default), switch the pin back to input. That is high impedance, with no pull-up and no pull-down.

GPIO16 has no open-drain mode and no pull-up. Its input mode must be `INPUT`. `INPUT_PULLDOWN_16` would hold the Stop pad down and look like a continuous press, so it is never used.

Only one button is driven at a time. A Stop for the remote that is currently pulsing cancels that pulse immediately and pulses Stop, with no extra gap. Any other command waits until the pulse has finished and an inter-command gap has elapsed (300 ms by default), including a Stop for the other remote. The gap reduces the chance of the two radios transmitting over each other. Pulse length, gap, pin assignments, cover names, and the MQTT prefix are in `include/config.h`.

The command queue keeps at most one pending command per remote. A new Open or Close replaces a pending Open or Close. A Stop replaces whatever is pending for that remote and jumps ahead of the other remote's Open or Close. Further Open or Close commands that do not fit are discarded. A Stop that does not fit discards the oldest non-Stop first. Serial logs every discard.

Wi-Fi and MQTT reconnects are timer-based. They are not tight loops, and a reconnect is not started while a button is held, so a dropped network does not leave a pad driven. On the ESP8266, one MQTT connect attempt can still block until the TCP timeout; that attempt waits until the pad has been released.

This firmware cannot claim a pin state during reset, deep sleep, or before `setup()` runs. The ROM bootloader and the Arduino core configure pins first. After `setup()` starts, all six button pins are set to released before serial or Wi-Fi initialisation. Power-on and reset glitches are outside what this program can prevent.

## Bench test before connecting the remotes

Leave the remotes disconnected for the first test. Upload, open the serial monitor at 115200, and send Open, Close, and Stop (from Home Assistant, or with `mosquitto_pub` once MQTT is up).

Each press logs `press remote N ACTION GPIOx` and, 200 ms later, `release remote N GPIOx`. While the log says press, that GPIO measures about 0 V. After release it is not driven: a meter can float, and the pin must not be driven high. Stop during an Open or Close on the same remote should release the first GPIO immediately and drive only the Stop GPIO.

Connect the remotes only after those pulses look right and the pad voltages have been checked.

## Build and upload

Install [PlatformIO](https://platformio.org/). From this directory:

```powershell
copy include\secrets.example.h include\secrets.h
```

Edit `include/secrets.h` with the Wi-Fi SSID, Wi-Fi password, and MQTT broker. Leave `MQTT_USER` as `""` when the broker does not use authentication. `include/secrets.h` is gitignored; do not commit it.

```powershell
pio run
pio run -t upload
pio device monitor
```

The monitor prints the chip id and the two command topics once the serial port is open.

## Home Assistant

The broker needs the MQTT integration. Discovery uses the default prefix `homeassistant`, and the firmware subscribes to the default birth topic `homeassistant/status` with payload `online`. Both can be changed in `include/config.h` if the integration was reconfigured.

On MQTT connect, and again whenever that birth message arrives, the device publishes:

- a retained availability topic, `{prefix}/{chip}/availability`, payload `online`
- a retained discovery config for each cover

The last will is retained `offline` on the availability topic, published by the broker if the controller disconnects uncleanly. Both covers use one device id, `velux_{chip}`, so Home Assistant groups them. The chip id is the ESP8266 chip id printed at boot, as eight hex digits.

Command topics are `{prefix}/{chip}/1/set` and `{prefix}/{chip}/2/set`. Payloads are `OPEN`, `CLOSE`, and `STOP`. The firmware does not subscribe to those topics until availability and both discovery messages have been published. It never publishes a command, and it never sets the retain flag on a command.

Clear any retained messages already stored on the command topics before the first connection. On subscribe, the broker replays a retained command and the controller will pulse that button. Replace `CHIP` with the eight-digit id from the serial log (or clear the topics again after the first boot log):

```powershell
mosquitto_pub -h BROKER -t "velux/CHIP/1/set" -r -n
mosquitto_pub -h BROKER -t "velux/CHIP/2/set" -r -n
```

A retained empty payload deletes the retained command. Do the same if the topic prefix in `include/config.h` is not `velux`.

The cover config sets `optimistic` true and does not set a state topic or a position topic. Home Assistant moves its own icon when it sends a command. Nothing in this device measures the window, and a button pressed on the remote itself is invisible to Home Assistant.
