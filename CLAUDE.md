# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

ESP32-based MQTT IR blaster with dynamic command storage. Commands are stored as MQTT retained messages instead of hardcoded in firmware, enabling zero-reflash IR command management.

**Key Architecture:**
- Commands stored on MQTT broker as retained messages (`home/ir/1/commands/*`)
- ESP32 loads commands into RAM cache on boot from MQTT subscriptions
- Learning mode captures IR signals and automatically publishes to MQTT
- Supports both known protocols (Samsung, NEC, LG, etc.) and raw timing data
- Automatic burst detection: captures when remotes send multiple IR signals per button press

## Hardware Configuration

```cpp
// ir_blaster.ino:84-85
constexpr uint8_t IR_SEND_PIN = 13;      // IR LED transmitter
constexpr uint8_t IR_RECEIVE_PIN = 27;   // IR receiver (TSOP38238)
const uint8_t ONBOARD_LED = 2;           // Status indicator
```

WiFi/MQTT credentials are hardcoded in `ir_blaster.ino:7-12`. Update these before flashing.

## Building and Flashing

**Requirements:**
- Arduino IDE with ESP32 board support
- Libraries: `PubSubClient`, `IRremote` (by ArminJo), `ArduinoJson` v6.x

**Build/Upload:**
1. Open `ir_blaster.ino` in Arduino IDE
2. Select board: **ESP32 Dev Module**
3. Update WiFi/MQTT credentials (lines 7-12)
4. Upload firmware (Ctrl+U / Cmd+U)
5. Open Serial Monitor (115200 baud) to verify connection

**Initial Command Migration:**
```bash
cd /Users/mattcarlos/Documents/Arduino/ir_blaster
python3 -m venv venv
source venv/bin/activate  # or venv\Scripts\activate on Windows
pip install paho-mqtt
python3 migrate_commands.py
```

This publishes 8 hardcoded commands (4 TV, 4 fan) as MQTT retained messages.

## Command Storage Architecture

### In-Memory Cache (`ir_blaster.ino:28-54`)
```cpp
#define MAX_COMMANDS 30           // Maximum cached commands
#define MAX_RAW_DATA 200          // Max timing values per raw command
#define MAX_COMMAND_NAME 32       // Max command name length

struct StoredCommand {
  char name[MAX_COMMAND_NAME];
  bool isRaw;                     // Protocol vs raw data
  uint8_t repeatCount;            // Captured burst count (0 = single)
  uint16_t repeatInterval;        // Milliseconds between bursts
  union {
    struct { /* protocol data */ } protocol;
    struct { /* raw timing array */ } raw;
  };
};
```

**Key Limits:**
- 30 commands total (configurable via `MAX_COMMANDS`)
- 200 raw timing values per command (configurable via `MAX_RAW_DATA`)
- ~20-30KB RAM for command cache
- Commands load from MQTT on boot via retained message subscription

### MQTT Topic Structure

| Topic | Direction | Payload | Purpose |
|-------|-----------|---------|---------|
| `home/ir/1/send` | HA → ESP | `"command_name"` | Execute cached command |
| `home/ir/1/listen` | HA → ESP | `{"name":"cmd_name"}` | Start learning mode (10s window) |
| `home/ir/1/learn` | ESP → HA | `{"name":"...","proto":"..."}` | Learning success log |
| `home/ir/1/state` | ESP → HA | Status strings | Boot, errors, learning events |
| `home/ir/1/commands/*` | Both | JSON command | Add/update/delete command (retained) |

## Key Code Flows

### Command Execution (`executeCommand()` at line 116)
1. Lookup command by name in cache
2. Calculate total bursts to send (1 + repeatCount)
3. For each burst:
   - If raw: `IrSender.sendRaw(data, len, freq)`
   - If protocol: Call protocol-specific sender (e.g., `IrSender.sendSamsung()`)
   - Delay `repeatInterval` ms before next burst
4. Publish success/error to `home/ir/1/state`

**Example:** A command with `repeatCount=5, repeatInterval=110` sends 6 total bursts spaced 110ms apart, matching the original remote's pattern.

### Learning Mode (`handleLearnWindow()` at line 637)
**Trigger:** MQTT message to `home/ir/1/listen` with JSON `{"name":"command_name"}`

**Flow:**
1. Set `learnActive = true`, start 10s timer
2. Wait for first IR signal → store as `baseSignal`
3. For 500ms idle timeout or 10s total timeout:
   - Detect additional bursts if signal matches `baseSignal`
   - Track burst count and inter-burst intervals
4. On timeout/idle (500ms with no signal):
   - Calculate average burst interval
   - Publish command to `home/ir/1/commands/{name}` (retained)
   - Add to ESP32 cache
5. Turn off receiver, reset state

**Burst Detection (`signalsMatch()` at line 618):**
- **Purpose:** Many remotes send 2-6 identical IR bursts per button press (not user holding)
- Protocol commands: Compare protocol + address + command
- Raw commands: Compare raw data length
- Captured bursts stored as `repeatCount` (additional bursts beyond first) and `repeatInterval` (ms between bursts)
- **Example:** Samsung TV volume sends 6 bursts at ~110ms intervals per single button press

### MQTT Message Routing (`onMqttMessage()` at line 315)
```cpp
if (strcmp(topic, TOPIC_LISTEN) == 0)        → Start learning mode
if (strcmp(topic, TOPIC_IR_SEND) == 0)       → Execute command by name
if (strncmp(topic, "home/ir/1/commands/") == 0) {
  if (len == 0) → Delete command
  else          → Add/update command in cache
}
```

## Command JSON Formats

**Protocol command (with bursts):**
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
*Sends 6 total bursts: initial + 5 additional at 110ms intervals*

**Raw command (with bursts):**
```json
{
  "raw": true,
  "freq": 38,
  "data": [1330, 270, 1380, ...],
  "repeatCount": 3,
  "repeatInterval": 115
}
```
*Sends 4 total bursts: initial + 3 additional at 115ms intervals*

**Single-burst command:**
```json
{
  "proto": "NEC",
  "addr": 0,
  "cmd": 0,
  "rpt": 0,
  "repeatCount": 0,
  "repeatInterval": 0
}
```
*Sends once only*

**Backward compatibility:** Commands without `repeatCount`/`repeatInterval` default to 0 (single burst).

## Common Tasks

### Add a new command via learning
```bash
# Via MQTT
mosquitto_pub -t 'home/ir/1/listen' -m '{"name":"bedroom_tv_input"}'
# Then press remote button ONCE within 10 seconds
# ESP32 automatically detects if remote sends burst pattern
# Wait 500ms for burst detection to complete
```

### Send a command
```bash
mosquitto_pub -t 'home/ir/1/send' -m 'tv_power'
```

### View all cached commands
```bash
mosquitto_sub -t 'home/ir/1/commands/#' -v
```

### Delete a command
```bash
# Publish empty retained message
mosquitto_pub -t 'home/ir/1/commands/unwanted' -n -r
```

### Manually add a command
```bash
mosquitto_pub -t 'home/ir/1/commands/test' \
  -m '{"proto":"NEC","addr":0,"cmd":0,"rpt":0}' -r
```

### Monitor ESP32 status
```bash
mosquitto_sub -t 'home/ir/1/state' -v
```

## Debugging

**Serial Monitor (115200 baud) output:**
- Boot: "ESP32 IR Controller Ready"
- MQTT connect: "MQTT connected! Loaded X commands from MQTT"
- Command execution: "Executing command: {name}"
- Learning: "First signal captured, listening for bursts..."
- Errors: "ERR:NOT_FOUND:command_name"

**Common Error Codes (published to `home/ir/1/state`):**
- `ERR:NOT_FOUND:{name}` - Command not in cache
- `ERR:CACHE_FULL` - Exceeded MAX_COMMANDS (30)
- `ERR:INVALID_JSON` - Malformed listen payload
- `ERR:NO_NAME` - Empty command name in learning request

**Memory Issues:**
If ESP32 crashes with many raw commands, reduce `MAX_RAW_DATA` (line 31) or `MAX_COMMANDS` (line 29).

## Home Assistant Integration

**Configuration files:** See `HOME_ASSISTANT_SETUP.md` and `scripts.yaml`

**Key scripts:**
- `ir_learn_command` - Trigger learning mode
- `ir_send_command` - Send any command by name
- Specific scripts: `ir_tv_power`, `ir_tv_vol_up`, etc.

**Input helper:** `input_text.ir_command_name` - Stores name for learning mode

## Important Notes

1. **Always use retained flag (`-r`) when publishing commands** - ESP32 loads them on boot
2. **Learning mode has two timeouts:**
   - 10s max total learning time
   - 500ms idle timeout after last signal (for burst detection)
3. **Burst detection:** Automatic - press button ONCE and ESP32 detects if remote sends multiple bursts (common for volume/channel controls). No need to hold button.
4. **Burst patterns vary by remote:**
   - Single-burst: Power buttons, input selection (1 signal per press)
   - Multi-burst: Volume, channel buttons (2-6 identical signals per press)
   - Check Serial Monitor during learning to see detected pattern
5. **Protocol parsing** happens in `parseProto()` (line 103) and `executeCommand()` switch statement (line 168)
6. **IRremote library version:** Code uses IRremote 4.x API (`IrReceiver.irparams.rawbuf`)
7. **No git repository** - This is a standalone Arduino project directory

## Burst Detection Feature

**Key insight:** Many IR remotes send multiple identical signals per button press (not from user holding). This is critical for proper command replay.

**How it works:**
1. User presses button ONCE on remote
2. Remote transmits 2-6 identical IR bursts in rapid succession (~50-500ms apart)
3. ESP32 detects first burst, stores as `baseSignal`
4. ESP32 listens for 500ms, detecting matching bursts
5. Calculates average inter-burst interval
6. Stores as `repeatCount` (additional bursts) and `repeatInterval` (ms between)
7. On replay: sends identical burst pattern to match original remote

**Why this matters:**
- Without burst detection: Multi-burst commands fail (only 1 of 6 signals sent)
- With burst detection: Commands work identically to physical remote
- Automatic: No user configuration needed

**Common patterns (from testing):**
- Samsung TV volume/channel: 6 bursts at ~110ms intervals
- Power buttons: Usually 1 burst (no repeats)
- Generic fans: 4 bursts at ~115ms intervals

**See `REPEAT_CAPTURE_FEATURE.md` for detailed documentation.**

## Code Organization

```
ir_blaster/
├── ir_blaster.ino           # Main firmware (783 lines)
│   ├── StoredCommand struct (line 33)
│   ├── MQTT callback routing (line 315)
│   ├── Command execution (line 116)
│   ├── Learning mode logic (line 637)
│   └── Burst detection (line 618)
├── migrate_commands.py      # One-time MQTT migration script
├── scripts.yaml             # Home Assistant scripts (for reference)
├── HOME_ASSISTANT_SETUP.md  # Detailed HA integration guide
├── REPEAT_CAPTURE_FEATURE.md # Documentation of burst detection feature
└── README.md                # User-facing documentation
```

## Adding New Protocol Support

To add a new IR protocol (e.g., "Denon"):

1. Add enum to `Proto` (line 88):
   ```cpp
   enum class Proto : uint8_t { ..., Denon };
   ```

2. Add parser in `parseProto()` (line 103):
   ```cpp
   if (strcasecmp(protoStr, "Denon") == 0) return Proto::Denon;
   ```

3. Add sender in `executeCommand()` switch (line 168):
   ```cpp
   case Proto::Denon: IrSender.sendDenon(addr, command, repeats); break;
   ```
