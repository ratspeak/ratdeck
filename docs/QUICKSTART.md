# Ratdeck — Quick Start

## Hardware Required

- **LilyGo T-Deck Plus** (ESP32-S3, 16MB flash, 8MB PSRAM, integrated SX1262 LoRa)
- **microSD card** (optional, recommended — FAT32, any size)
- **USB-C cable** (data-capable)

## Build & Flash

```bash
# Install PlatformIO
pip install platformio

# Clone and build
git clone https://github.com/ratspeak/ratdeck.git
cd Ratdeck
python3 -m platformio run

# Flash
python3 -m platformio run --target upload
```

> If `pio` is not on your PATH, use `python3 -m platformio` instead. See [BUILDING.md](BUILDING.md) for esptool instructions and troubleshooting.

### Web Flash (No Build Required)

Visit [ratspeak.org/download](https://ratspeak.org/download.html) to flash directly from your browser using WebSerial.

### USB Port

The T-Deck Plus uses USB-Serial/JTAG — port appears as `/dev/cu.usbmodem*` (macOS) or `/dev/ttyACM*` (Linux).

## First Boot

1. Power on the T-Deck Plus
2. Boot animation plays with progress bar
3. SX1262 radio initializes (915 MHz, Balanced preset)
4. SD card checked — auto-creates `/ratputer/` directories
5. Reticulum identity generated (Ed25519 keypair, saved to flash + SD + NVS)
6. Name input screen — type a display name or press Enter to auto-generate one
7. Home screen shows your name, LXMF address, connection status, and online node count

## Navigation

The T-Deck Plus has a QWERTY keyboard, trackball, and touchscreen (touch currently disabled):

| Input | Action |
|-------|--------|
| Trackball up/down | Scroll lists |
| Trackball left/right | Cycle tabs |
| Trackball click | Select / confirm |
| Trackball long-press (1.2s) | Context menu |
| `,` / `/` | Previous / next tab |
| Enter | Select / send message |
| Esc or Backspace | Back / cancel |
| Ctrl+H | Show hotkey help overlay |

### Tabs

| Tab | Name | Contents |
|-----|------|----------|
| 1 | Home | Your name, LXMF address, connection status, online nodes |
| 2 | Friends | Saved contacts with display names |
| 3 | Msgs | Conversations sorted by most recent, with preview and unread dots |
| 4 | Peers | All discovered nodes — contacts section + online section |
| 5 | Setup | Device, Display, Radio, Network, Audio, Info, System |

## Sending Your First Message

1. Wait for another node to appear in the **Peers** tab (or connect via WiFi)
2. Navigate to the node and press **Enter** to open a chat
3. Type your message on the keyboard
4. Press **Enter** to send
5. Message status: yellow (sending) -> green (delivered) or red (failed)

## Connecting Two Ratdecks Over LoRa

1. Power on both devices
2. Ensure both use the same radio settings (defaults work out of the box)
3. Wait ~30 seconds for announces to propagate
4. The other device appears in the **Peers** tab with RSSI and SNR
5. Select the node and press Enter to start chatting

Both devices must use the same frequency, spreading factor, bandwidth, and coding rate. Default: 915 MHz, SF9, BW 125 kHz, CR 4/5 (Balanced preset).

## WiFi Setup

Default WiFi mode is **STA** (client). Change in Settings -> Network.

### STA Mode (Connect to Your WiFi)

1. Go to Setup -> Network
2. Enter your WiFi SSID and password
3. Add a TCP endpoint (e.g., `rns.beleth.net:4242`)
4. Ratdeck connects and bridges LoRa <-> TCP automatically

### AP Mode (Ratdeck as Hotspot)

1. Set WiFi Mode to AP in Settings -> Network
2. Connect your laptop to `ratdeck-XXXX` (password: `ratspeak`)
3. Add to `~/.reticulum/config`:
   ```ini
   [[ratdeck]]
     type = TCPClientInterface
     target_host = 192.168.4.1
     target_port = 4242
   ```
4. Your desktop is now bridged to the LoRa mesh

### WiFi OFF

Select OFF in Settings -> Network to disable WiFi entirely (saves power).

## SD Card

Insert a microSD card (FAT32) before powering on. The firmware auto-creates:

```
/ratputer/
  config/           Settings backup
  messages/         Message archives (up to 100 per conversation)
  contacts/         Saved friends
  identity/         Identity key backup
  transport/        Routing table backup
```

## Managing Friends

- **Peers tab**: Navigate to a node, press `s` to toggle friend status, or long-press to add/delete
- **Messages tab**: Long-press a conversation for context menu (Add Friend / Delete Chat / Cancel)
- Friends are saved to flash + SD and persist across reboots
- Friends are never auto-evicted from the node list

## Radio Presets

Three presets available in Setup -> Radio:

| Preset | SF | BW | CR | TX | Use Case |
|--------|----|----|----|----|----------|
| **Balanced** (default) | 9 | 125 kHz | 4/5 | 17 dBm | General use |
| Long Range | 12 | 62.5 kHz | 4/8 | 22 dBm | Maximum range, slow |
| Fast | 7 | 250 kHz | 4/5 | 14 dBm | Short range, fast |

All parameters are individually configurable. Changes apply immediately.

## Serial Diagnostics

Connect at 115200 baud for debug output:

```bash
python3 -m platformio device monitor -b 115200
```

- **Ctrl+D** on device: full diagnostics dump (identity, radio, heap, uptime)
- **Ctrl+T** on device: send radio test packet
- **Ctrl+R** on device: 5-second RSSI sampling

## Transport Mode

By default, Ratdeck runs as an **Endpoint** (handles its own traffic only). Enable **Transport Node** in Setup -> Network to relay packets for other nodes in the mesh. Requires reboot.
