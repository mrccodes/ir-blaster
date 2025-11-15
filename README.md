# MQTT IR Blaster with Dynamic Command Storage

ESP32-based IR blaster that stores all commands as MQTT retained messages, eliminating the need to reflash firmware when adding new IR codes.

## Features

✅ **No firmware reflashing** - Add commands via MQTT
✅ **Learning mode** - Capture IR codes with simple names
✅ **Automatic burst detection** - Captures multi-signal button presses automatically
✅ **Protocol support** - Samsung, NEC, LG, Sony, JVC, RC5, RC6, Panasonic
✅ **Raw IR support** - Handles unknown/proprietary protocols
✅ **Home Assistant integration** - Native MQTT control
✅ **Command caching** - ESP32 loads commands from broker on boot
✅ **Persistent storage** - Commands survive reboots (stored on MQTT broker)
✅ **Secure credentials** - WiFi/MQTT passwords stored in separate file

## What's New

### Automatic Burst Detection
Many IR remotes send multiple identical signals per button press (not from holding the button). The ESP32 now automatically detects these burst patterns during learning and replays them perfectly. No configuration needed!

**Example:** Samsung TV volume buttons send 6 bursts spaced 110ms apart per single press - captured and replayed automatically.

### Recent Improvements
- **Increased MQTT buffer** - Now supports large raw commands (2048 bytes)
- **Fixed JSON parsing** - Properly handles commands with 200+ timing values
- **Credentials security** - WiFi/MQTT passwords moved to separate `credentials.h` file

## Quick Start

### 1. Hardware Setup

**Required Components:**
- ESP32 development board
- IR LED (940nm recommended)
- IR receiver module (TSOP38238 or similar 38kHz receiver)
- 330Ω resistor for IR LED
- Optional: 2N2222 transistor for stronger IR transmission

**Connections:**
```
IR LED (Transmitter):
  GPIO 13 ──┬── 330Ω resistor ── IR LED anode (+)
            │
            └── Optional: Transistor for more power

  IR LED cathode (-) ── GND

IR Receiver:
  IR Receiver OUT ── GPIO 27
  IR Receiver VCC ── 3.3V
  IR Receiver GND ── GND

Status LED:
  GPIO 2 (built-in LED on most ESP32 boards)
  - ON during learning mode
  - OFF when idle
```

### 2. Software Setup

**Install PlatformIO:**
- [VS Code](https://code.visualstudio.com/) + [PlatformIO extension](https://platformio.org/install/ide?install=vscode)
- Or use [PlatformIO CLI](https://docs.platformio.org/en/latest/core/installation.html)

**Clone/Download Project:**
```bash
cd /path/to/PlatformIO/Projects
# (or your PlatformIO projects directory)
```

### 3. Configure Credentials

**Create your credentials file:**
```bash
cd "IR Blaster"
cp src/credentials.h.example src/credentials.h
```

**Edit `src/credentials.h`** with your details:
```cpp
#define WIFI_SSID     "YourWiFiSSID"
#define WIFI_PASS     "YourWiFiPassword"
#define MQTT_HOST     "homeassistant.local"  // Or IP: "192.168.1.100"
#define MQTT_PORT     1883
#define MQTT_USER     "your_mqtt_username"
#define MQTT_PASS     "your_mqtt_password"
#define MQTT_CLIENTID "esp32-ir-1"  // Unique ID if you have multiple devices
```

> **Note:** `credentials.h` is in `.gitignore` - your passwords won't be committed to git!

### 4. Build and Upload

**Via PlatformIO (VS Code):**
1. Open project folder in VS Code
2. Click "Build" button (checkmark icon) in status bar
3. Click "Upload" button (arrow icon)
4. Open Serial Monitor (plug icon, 115200 baud)

**Via PlatformIO CLI:**
```bash
cd "IR Blaster"
pio run --target upload
pio device monitor
```

**Expected Serial Output:**
```
WiFi connected
MQTT connected!
Subscribed to topics
Loaded X commands from MQTT
ESP32 IR Controller Ready
```

### 5. Migrate Existing Commands (Optional)

If you have hardcoded commands to migrate:

```bash
cd "IR Blaster"
python3 -m venv venv
source venv/bin/activate  # On Windows: venv\Scripts\activate
pip install paho-mqtt
python3 migrate_commands.py
```

This publishes 8 example commands (TV and fan controls) as MQTT retained messages.

### 6. Home Assistant Integration

See [HOME_ASSISTANT_SETUP.md](HOME_ASSISTANT_SETUP.md) for complete configuration.

**Quick setup** - Add to `configuration.yaml`:
```yaml
input_text:
  ir_command_name:
    name: "New IR Command Name"
    max: 32
```

**Add to `scripts.yaml`:**
```yaml
ir_learn_command:
  alias: "Learn IR Command"
  sequence:
    - service: mqtt.publish
      data:
        topic: "home/ir/1/listen"
        payload: '{"name":"{{ states(''input_text.ir_command_name'') }}"}'
    - service: notify.persistent_notification
      data:
        title: "IR Learning Started"
        message: "Point remote at sensor and press button within 10 seconds"
```

Reload Home Assistant configuration.

## Usage

### Send a Command

**Via MQTT:**
```bash
mosquitto_pub -t 'home/ir/1/send' -m 'tv_power'
```

**Via Home Assistant:**
```yaml
service: script.ir_tv_power
# Or use the generic script:
service: script.ir_send_command
data:
  command: "tv_power"
```

### Learn a New Command

**Via Home Assistant:**
1. Enter command name in the input field (e.g., `bedroom_tv_input`)
2. Run the `ir_learn_command` script
3. Point remote at IR receiver within 10 seconds
4. Press button **once** on the remote
5. Wait for "Command learned successfully" notification

**Via MQTT:**
```bash
# Start learning mode
mosquitto_pub -t 'home/ir/1/listen' -m '{"name":"bedroom_tv_input"}'

# Point remote and press button within 10 seconds

# Monitor status
mosquitto_sub -t 'home/ir/1/state' -v
```

**What happens during learning:**
- ESP32 captures first IR signal
- Automatically detects if remote sends multiple bursts (common for volume/channel buttons)
- Waits 500ms for additional bursts
- Calculates average burst interval
- Saves complete pattern to MQTT as retained message
- Adds to ESP32 RAM cache immediately

### View All Commands

```bash
mosquitto_sub -t 'home/ir/1/commands/#' -v
```

**Example output:**
```
home/ir/1/commands/tv_power {"proto":"Samsung","addr":7,"cmd":2,"rpt":0,"repeatCount":0,"repeatInterval":0}
home/ir/1/commands/tv_vol_up {"proto":"Samsung","addr":7,"cmd":7,"rpt":0,"repeatCount":5,"repeatInterval":110}
home/ir/1/commands/fan_power {"raw":true,"freq":38,"data":[1330,270,1380,...],"repeatCount":0,"repeatInterval":0}
```

Note: `repeatCount` and `repeatInterval` are automatically set based on detected burst patterns.

### Delete a Command

```bash
# Publish empty retained message
mosquitto_pub -t 'home/ir/1/commands/unwanted_command' -n -r

# ESP32 will remove it from cache automatically
```

### Manually Add a Command

**Known protocol:**
```bash
mosquitto_pub -t 'home/ir/1/commands/test_nec' \
  -m '{"proto":"NEC","addr":0,"cmd":0,"rpt":0,"repeatCount":0,"repeatInterval":0}' -r
```

**Raw timing data:**
```bash
mosquitto_pub -t 'home/ir/1/commands/test_raw' \
  -m '{"raw":true,"freq":38,"data":[1330,270,1380,270],"repeatCount":0,"repeatInterval":0}' -r
```

## Command Formats

### Protocol Command (Known IR Protocols)

```json
{
  "proto": "Samsung",
  "addr": 7,
  "cmd": 2,
  "rpt": 0,
  "repeatCount": 5,
  "repeatInterval": 110
}
```

**Fields:**
- `proto` - Protocol name (Samsung, NEC, LG, Sony12, JVC, RC5, RC6, Panasonic)
- `addr` - Device address (protocol-specific)
- `cmd` - Command code (protocol-specific)
- `rpt` - Legacy repeat count (use 0)
- `repeatCount` - Number of additional bursts to send (0 = single burst)
- `repeatInterval` - Milliseconds between bursts

### Raw Command (Unknown Protocols)

```json
{
  "raw": true,
  "freq": 38,
  "data": [1330, 270, 1380, 270, 580, 1220, ...],
  "repeatCount": 3,
  "repeatInterval": 115
}
```

**Fields:**
- `raw` - Must be `true`
- `freq` - Carrier frequency in kHz (usually 38)
- `data` - Array of timing values in microseconds (max 200 values)
- `repeatCount` - Number of additional bursts
- `repeatInterval` - Milliseconds between bursts

Used automatically when learning unknown IR protocols.

## MQTT Topics

| Topic | Direction | Payload | Description |
|-------|-----------|---------|-------------|
| `home/ir/1/send` | HA → ESP | `"tv_power"` | Send command by name |
| `home/ir/1/listen` | HA → ESP | `{"name":"cmd"}` | Start 10s learning window |
| `home/ir/1/learn` | ESP → HA | `{"name":"...","proto":"..."}` | Learned command log (non-retained) |
| `home/ir/1/state` | ESP → HA | Status messages | Boot, errors, learning events |
| `home/ir/1/commands/*` | Both | Command JSON | Command definitions (retained) |

**State messages:**
- `online (loaded X commands)` - Boot complete
- `learn_start:command_name` - Learning mode started
- `learn_burst_detected:N` - Detected Nth burst during learning
- `learn_success:name` or `learn_success:name,bursts:N` - Command saved
- `learn_timeout:no_signal` - No IR signal received in 10s
- `ERR:NOT_FOUND:name` - Command not in cache
- `ERR:CACHE_FULL` - Exceeded MAX_COMMANDS (30)
- `ERR:INVALID_JSON` - Malformed JSON payload

## Project Structure

```
IR Blaster/
├── src/
│   ├── main.cpp                  # Main ESP32 firmware (785 lines)
│   ├── credentials.h             # WiFi/MQTT credentials (gitignored)
│   └── credentials.h.example     # Template for credentials
├── platformio.ini                # PlatformIO configuration
├── .gitignore                    # Excludes credentials.h
├── migrate_commands.py           # Script to publish initial commands
├── README.md                     # This file
├── CLAUDE.md                     # Detailed architecture documentation
├── HOME_ASSISTANT_SETUP.md       # Home Assistant integration guide
└── REPEAT_CAPTURE_FEATURE.md     # Burst detection feature docs
```

## Troubleshooting

### Command Not Working (Fan/TV doesn't respond)

**Symptom:** Command sends but device doesn't respond.

**Check Serial Monitor output:**
```
Executing command: fan_power
Sending raw command, freq=38, len=95
Command sent successfully
```

**If `len` is too small (e.g., `len=3` instead of `len=95`):**
- This was a bug in earlier versions (MQTT buffer too small)
- **Fix:** Update to latest firmware (includes `mqtt.setBufferSize(2048)`)
- Delete and re-learn the command

**If length looks correct but still doesn't work:**
- IR LED might be too weak - add transistor amplifier
- Check IR LED polarity (anode to GPIO 13 via resistor, cathode to GND)
- Try learning command again (some remotes have variations)
- Ensure IR LED is 940nm wavelength

### ESP32 Not Loading Commands

**Check:**
- MQTT broker is running: `mosquitto_sub -t '#' -v`
- ESP32 can connect (Serial: "MQTT connected!")
- Commands are retained: `mosquitto_sub -t 'home/ir/1/commands/#' -v`
- Serial shows "Loaded X commands from MQTT"

**Fix:**
```bash
# Re-publish commands with retained flag
mosquitto_pub -t 'home/ir/1/commands/tv_power' \
  -m '{"proto":"Samsung","addr":7,"cmd":2,"rpt":0,"repeatCount":0,"repeatInterval":0}' -r
```

### Learning Mode Not Starting

**Symptoms:**
- No "learn_start" message on `home/ir/1/state`
- No LED turn on

**Check:**
- JSON payload format correct: `{"name":"command_name"}`
- Command name not empty
- Not already in learning mode (10s cooldown)

**Test:**
```bash
mosquitto_pub -t 'home/ir/1/listen' -m '{"name":"test"}'
# Check Serial Monitor for "Learning mode activated"
```

### Learning Times Out / No Signal Detected

**Symptoms:**
- Serial: "learn_timeout:no_signal"
- No IR signal captured

**Check:**
- IR receiver wiring (especially VCC to 3.3V, not 5V)
- IR receiver facing the remote (not blocked)
- Remote has working batteries
- Pressing button within 10 second window
- Try different remote/button (some remotes are weak)

### Command Not Found Error

**Symptoms:**
- Serial: "Command not found: xxx"
- State topic: "ERR:NOT_FOUND:xxx"

**Check:**
- Command exists: `mosquitto_sub -t 'home/ir/1/commands/#' -v`
- Name matches exactly (case-sensitive: `tv_power` ≠ `TV_Power`)
- Command was published with `-r` (retained) flag
- ESP32 has restarted since command was added

**Fix:**
```bash
# Check if command exists on broker
mosquitto_sub -t 'home/ir/1/commands/tv_power' -v

# If missing, re-publish with -r flag
mosquitto_pub -t 'home/ir/1/commands/tv_power' \
  -m '{"proto":"Samsung","addr":7,"cmd":2,"rpt":0,"repeatCount":0,"repeatInterval":0}' -r
```

### Memory Issues / Cache Full

**Symptoms:**
- Serial: "ERR:CACHE_FULL"
- ESP32 crashes during command load
- Can't learn new commands

**Solution 1 - Delete unused commands:**
```bash
mosquitto_pub -t 'home/ir/1/commands/unused_command' -n -r
```

**Solution 2 - Increase limits (requires reflash):**

Edit `src/main.cpp` around line 29-31:
```cpp
#define MAX_COMMANDS 30    // Increase to 50 (uses more RAM)
#define MAX_RAW_DATA 200   // Decrease to 100 if needed
```

Rebuild and upload firmware.

### Home Assistant Notification Not Appearing

**Symptom:** Learning works but no "Command learned successfully" notification in HA.

**Check:**
- Automation listening to `home/ir/1/learn` topic exists (see `HOME_ASSISTANT_SETUP.md`)
- MQTT integration configured in Home Assistant
- Test manually: `mosquitto_sub -t 'home/ir/1/learn' -v` and learn a command

**Fix:** Add automation from `HOME_ASSISTANT_SETUP.md` lines 122-142.

## System Limits

| Item | Limit | Configurable |
|------|-------|--------------|
| Max commands | 30 | Yes (`MAX_COMMANDS`) |
| Max raw timing values | 200 per command | Yes (`MAX_RAW_DATA`) |
| Command name length | 31 characters | Yes (`MAX_COMMAND_NAME`) |
| MQTT packet size | 2048 bytes | Yes (`mqtt.setBufferSize()`) |
| Learning window | 10 seconds | Yes (line 711) |
| Burst detection timeout | 500ms idle | Yes (line 710) |
| RAM usage (approx) | ~30KB for cache | Varies with limits |

## Advanced Usage

### Backup All Commands

```bash
# Export to file
mosquitto_sub -t 'home/ir/1/commands/#' -v > ir_backup.txt

# Restore by parsing file and re-publishing each line with -r flag
```

### Multi-Device Setup

For multiple IR blasters:

**Device 1:**
```cpp
#define MQTT_CLIENTID "esp32-ir-1"
// Topics: home/ir/1/*
```

**Device 2:**
```cpp
#define MQTT_CLIENTID "esp32-ir-2"
// Update topics in main.cpp to: home/ir/2/*
```

### Voice Control via Home Assistant

**Alexa/Google Home example:**
```yaml
# configuration.yaml
intent_script:
  TurnOnTV:
    speech:
      text: "Turning on TV"
    action:
      service: mqtt.publish
      data:
        topic: "home/ir/1/send"
        payload: "tv_power"
```

### Over-The-Air (OTA) Updates

Add to `platformio.ini`:
```ini
upload_protocol = espota
upload_port = esp32-ir-1.local
```

Then:
```bash
pio run --target upload
```

## Performance Notes

- **Command execution:** < 50ms from MQTT message to IR transmission
- **Learning mode:** 10s maximum, auto-completes in 500ms after last burst
- **Boot time:** ~3 seconds (WiFi + MQTT + command load)
- **MQTT reconnect:** Automatic with exponential backoff
- **Command caching:** All commands loaded to RAM on boot (no MQTT required during sending)

## Security Considerations

- **Credentials:** Stored in separate `credentials.h` file (gitignored)
- **MQTT:** Uses username/password authentication
- **Network:** No web server exposed (MQTT-only interface)
- **Flash storage:** Commands stored on MQTT broker, not in ESP32 firmware

**Recommendations:**
- Use strong MQTT passwords
- Keep ESP32 on isolated IoT VLAN
- Enable MQTT TLS if your broker supports it (requires code changes)
- Regularly update firmware for security patches

## Contributing

This is a personal project but improvements are welcome!

**Bug reports:** Include Serial Monitor output and MQTT topic dumps
**Feature requests:** Describe use case and expected behavior
**Pull requests:** Test with real hardware before submitting

## License

MIT License - Use freely, attribution appreciated.

## Credits

- **IRremote library** by ArminJo - [GitHub](https://github.com/Arduino-IRremote/Arduino-IRremote)
- **PubSubClient** by Nick O'Leary - [GitHub](https://github.com/knolleary/pubsubclient)
- **ArduinoJson** by Benoit Blanchon - [GitHub](https://github.com/bblanchon/ArduinoJson)

---

## Further Reading

- [CLAUDE.md](CLAUDE.md) - Detailed architecture and code organization
- [HOME_ASSISTANT_SETUP.md](HOME_ASSISTANT_SETUP.md) - Complete Home Assistant integration
- [REPEAT_CAPTURE_FEATURE.md](REPEAT_CAPTURE_FEATURE.md) - Burst detection technical details

**Need help?** Check the troubleshooting section above or review the Serial Monitor output at 115200 baud.
