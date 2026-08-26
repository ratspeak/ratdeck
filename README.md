<div align="center">

# [rsDeck](https://ratspeak.org/)

**Dual-mode Ratspeak firmware for the LilyGo T-Deck Plus.**

[![Status](https://img.shields.io/badge/status-beta-yellow.svg)](#install)
[![License](https://img.shields.io/badge/license-AGPL--3.0--or--later-blue.svg)](LICENSE)
[![Version](https://img.shields.io/badge/version-2.0.2-success.svg)](https://github.com/ratspeak/rsDeck/releases)

[Ratspeak](https://github.com/ratspeak/Ratspeak) |
[Docs](https://ratspeak.org/docs.html) |
[Downloads](https://ratspeak.org/download.html) |
[rsReticulum](https://github.com/ratspeak/rsReticulum)

</div>

---

rsDeck turns a [LilyGo T-Deck Plus](https://www.lilygo.cc/products/t-deck-plus)
into a two-mode Reticulum handheld. Standalone mode is an on-device
Ratspeak/LXMF messenger. RNode mode makes the T-Deck a host-controlled radio
for Ratspeak, Sideband, or another Reticulum client over BLE or USB serial.

## Install

Use the Ratspeak web flasher:
[ratspeak.org/download.html](https://ratspeak.org/download.html).

Put the T-Deck Plus in download mode by holding the trackball while powering on,
connect USB, then flash `rsdeck-full`. The standalone-only and RNode-only
images are release artifacts for launcher users or focused testing.

## Modes

On boot, the launcher lets you choose:

- **Standalone**: a local Reticulum/LXMF messenger with identity management,
  contacts, peer discovery, messages, LoRa, and WiFi TCP access.
- **RNode**: a host-controlled RNode-style radio target for Ratspeak or other
  Reticulum clients over BLE or USB serial.

RNode mode self-provisions the T-Deck RNode product/model/default config and
running firmware hash on first boot, so users should not need a separate
`rnodeconf` setup step for the bundled release images.

## Basic Use

On first boot, Standalone mode generates a Reticulum identity and asks for a
display name. Your LXMF address is the 32-character hex string you share with
contacts.

- Tabs: Home, Friends, Msgs, Peers, Setup.
- Navigation: trackball movement and click/Enter.
- Announce: press the trackball or Enter on the Home tab.
- Add contacts: select a discovered peer, then open or save the chat.
- Send messages: open a chat, type, and press Enter.
- Delivery color: yellow while sending, green after delivery confirmation.
- Identity import: place one raw 64-byte Reticulum identity file under
  `/ratdeck/identity/` on the SD card, then use Setup -> Import Identity.
  Files named `import.identity` or `import.key` are preferred; otherwise rsDeck
  accepts exactly one non-reserved `.identity` or `.key` file in that folder.
  Ratspeak's encrypted `.rsi` backups are not imported on-device; from Ratspeak,
  export a Reticulum Identity File for rsDeck import.

## Radio Presets

`Long Fast` is the compiled-in default. All radio parameters can also be tuned
from Setup. Changes apply immediately.

| Preset | SF | BW | CR | TXP | Bitrate | Link budget |
|---|---:|---:|---:|---:|---:|---:|
| Short Turbo | 7 | 500 kHz | 4/5 | 14 dBm | 21.99 kbps | 140 dB |
| Short Fast | 7 | 250 kHz | 4/5 | 14 dBm | 10.84 kbps | 143 dB |
| Short Slow | 8 | 250 kHz | 4/5 | 14 dBm | 6.25 kbps | 145.5 dB |
| Medium Fast | 9 | 250 kHz | 4/5 | 17 dBm | 3.52 kbps | 148 dB |
| Medium Slow | 10 | 250 kHz | 4/5 | 17 dBm | 1.95 kbps | 150.5 dB |
| Long Turbo | 11 | 500 kHz | 4/8 | 22 dBm | 1.34 kbps | 150 dB |
| **Long Fast** *(default)* | **11** | **250 kHz** | **4/5** | **22 dBm** | **1.07 kbps** | **153 dB** |
| Long Moderate | 11 | 125 kHz | 4/8 | 22 dBm | 0.34 kbps | 156 dB |

You are responsible for operating within local laws and radio regulations.

## WiFi Bridging

WiFi bridging is experimental. STA mode can connect to existing WiFi and reach
remote Reticulum nodes such as `rns.ratspeak.org:4242`.

AP mode exposes a local TCP endpoint for a nearby Reticulum host:

```ini
[[rsdeck]]
  type = TCPClientInterface
  target_host = 192.168.4.1
  target_port = 4242
```

The bridging UI and interface behavior may change as Ratspeak's client release
stabilizes.

## Battery

The stock T-Deck Plus battery is read through a fixed ADC voltage-divider
ratio. Third-party/extended battery packs are commonly wired through a
different divider network on their sense line, which can make the reported
voltage (and therefore percentage) wrong even though the battery itself is
fine — e.g. reading ~69% when actually fully charged.

Tested and confirmed working with a
[makernova.io](https://makernova.io/) 8000mAh pack (JST 1.25 connector) in
[AlleyCat's extended T-Deck Plus case](https://www.printables.com/model/1345336)
(3D-printed, Printables #1345336).

If your battery percentage looks wrong, calibrate the ADC divider ratio
against a multimeter reading on the battery terminals (ideally with USB
disconnected, to avoid charge-line voltage bias):

```
b            # print raw ADC, computed voltage, divider ratio, percent
B<ratio>     # set + persist a new divider ratio, e.g. B1.8762
```

over the device's serial console (115200 baud). The firmware computes the
correct ratio for you from a live raw ADC reading + your multimeter value —
see the in-firmware `b` output for the exact formula. Settings persist across
reboots and firmware updates (stored alongside your identity/config, not
touched by a normal OTA/reflash).

The `Charge Above` and `Full Threshold` settings (Settings → Battery, hold
trackball click to reveal developer controls) are separate knobs:
`Charge Above` is the voltage above which the device assumes it's actively on
a charger (typically just under the LiPo absolute max of ~4.2V, not the
resting-full voltage); `Full Threshold` compensates the discharge curve for
your particular pack's typical resting-voltage sag under load. They're not
meant to be compared to each other directly.

## Build From Source

```bash
git clone https://github.com/ratspeak/rsDeck
cd rsDeck
python3 -m pip install platformio esptool
make prep-tdeck
make package
make flash port=/dev/cu.usbmodem3101
```

Useful build targets:

```bash
make build-launcher      # launcher only
make build-standalone    # standalone messenger app
make build-rnode         # host-controlled RNode target
make full-image          # launcher + Standalone + RNode
make package             # release zips and launcher bins
```

Release artifacts are written to `dist/`:

```text
dist/rsdeck-full.zip
dist/rsdeck-standalone.zip
dist/rsdeck-rnode.zip
dist/rsdeck-standalone-m5launcher.bin
dist/rsdeck-rnode-m5launcher.bin
```

Use the `.zip` files with the Ratspeak web flasher. The `*-m5launcher.bin`
files are app images for M5Launcher/M5Burner-style launchers that boot
Standalone or RNode directly from SD.

## License

rsDeck standalone firmware, launcher, partition tables, and packaging tools are
licensed under the GNU Affero General Public License v3.0 or later. See
[LICENSE](LICENSE).

Vendored third-party code keeps its own license notices, including
`vendor/rnode_firmware/` and `lib/Crypto`.
