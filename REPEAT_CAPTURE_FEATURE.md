# Automatic Burst Detection Feature

## Overview

The IR blaster automatically detects when remotes send multiple IR signal bursts for a single button press. Many remotes (especially for TVs, fans, and AC units) transmit the same code 2-6 times in rapid succession to ensure reliable reception. The ESP32 captures this burst pattern and replicates it exactly when replaying commands.

## How It Works

### The Problem

Different remotes have different transmission patterns:
- **Single-burst remotes:** Send IR code once per button press
- **Multi-burst remotes:** Send IR code 3-6 times per button press (common for volume, power, channel buttons)

Without burst detection, captured commands from multi-burst remotes would only replay once, making them unreliable or non-functional.

### The Solution

When learning a command, the ESP32:
1. Captures the first IR signal
2. Listens for 500ms to detect additional bursts
3. Measures the timing between bursts
4. Stores the burst count and interval
5. Replays commands with identical burst patterns

### What Gets Captured

For each command, the system stores:
- **Command data** (protocol info or raw timing)
- **repeatCount**: Number of additional bursts (0 = single burst)
- **repeatInterval**: Milliseconds between bursts

### Example: Volume Up Button

A typical TV remote volume+ button:
- **User action:** Press button ONCE
- **Remote sends:** 6 identical bursts spaced ~110ms apart
- **ESP32 detects:** 1 initial burst + 5 additional bursts
- **Saves:** `{"proto":"Samsung","addr":7,"cmd":7,"rpt":0,"repeatCount":5,"repeatInterval":110}`
- **On replay:** Sends 6 bursts spaced 110ms apart (exact match)

## Usage

### Learning a Command (Automatic Burst Detection)

**All button types - same process:**

1. In Home Assistant, enter command name: `tv_vol_up`
2. Click "Start Learning"
3. **Press button ONCE on your remote** (normal press, don't hold)
4. ESP32 automatically detects burst pattern within 500ms
5. Wait for learning to complete (indicated by timeout or LED off)

**Serial Monitor output for multi-burst remote:**
```
First signal captured, listening for bursts (500ms idle timeout)...
Burst #2 detected (interval: 108ms)
Burst #3 detected (interval: 110ms)
Burst #4 detected (interval: 109ms)
Burst #5 detected (interval: 111ms)
Burst #6 detected (interval: 110ms)
Burst sequence complete (500ms idle)
Captured 6 total bursts, avg interval: 110ms
Command saved to: home/ir/1/commands/tv_vol_up
```

**Serial Monitor output for single-burst remote:**
```
First signal captured, listening for bursts (500ms idle timeout)...
Burst sequence complete (500ms idle)
Single burst (no repeats)
Command saved to: home/ir/1/commands/tv_power
```

### How to Tell If Your Remote Uses Bursts

**Method 1: Serial Monitor**
Watch the Serial output during learning - if you see "Burst #2, #3, etc." messages, your remote uses bursts.

**Method 2: MQTT Message**
Check the learned command JSON:
- `"repeatCount": 0` → Single burst remote
- `"repeatCount": 5` → Multi-burst remote (6 total bursts)

**Method 3: State Topic**
Learning success message shows burst count:
- `learn_success:tv_power` → Single burst
- `learn_success:tv_vol_up,bursts:6` → Multi-burst

## JSON Format

### Protocol Command with Bursts
```json
{
  "proto": "Samsung",
  "addr": 7,
  "cmd": 7,
  "rpt": 0,
  "repeatCount": 5,
  "repeatInterval": 110
}
```
*(Sends 6 total: initial + 5 repeats at 110ms intervals)*

### Raw Command with Bursts
```json
{
  "raw": true,
  "freq": 38,
  "data": [1330, 270, 1380, ...],
  "repeatCount": 3,
  "repeatInterval": 115
}
```
*(Sends 4 total: initial + 3 repeats at 115ms intervals)*

### Single Burst Command (Backward Compatible)
```json
{
  "proto": "NEC",
  "addr": 34,
  "cmd": 12,
  "rpt": 0,
  "repeatCount": 0,
  "repeatInterval": 0
}
```
*(Sends 1 time only)*

**Note:** Commands without `repeatCount`/`repeatInterval` fields default to 0 (single burst).

## Behavior When Sending Commands

### Multi-Burst Command (repeatCount > 0)
1. ESP32 sends initial IR signal
2. Waits `repeatInterval` milliseconds
3. Sends identical signal again
4. Repeats until `1 + repeatCount` total signals sent

**Example Serial Output:**
```
Executing command: tv_vol_up
Will send 6 times with 110ms interval
Sending protocol command: Samsung
Sending repeat #1
Sending repeat #2
Sending repeat #3
Sending repeat #4
Sending repeat #5
Command sent successfully
```

### Single-Burst Command (repeatCount = 0)
1. ESP32 sends single IR signal
2. Done

## MQTT State Topics

During learning, the ESP32 publishes status updates to `home/ir/1/state`:

- `learn_start:{name}` - Learning mode started (10s window)
- `learn_burst_detected:{count}` - Additional burst captured
- `learn_success:{name},bursts:{count}` - Learning complete (multi-burst)
- `learn_success:{name}` - Learning complete (single-burst)
- `learn_timeout:no_signal` - No signal received within 10 seconds

## Benefits

### Reliability
✅ Commands work identically to physical remote
✅ Multi-burst remotes no longer unreliable
✅ Single-burst remotes unaffected (backward compatible)

### Automatic Detection
✅ No manual configuration required
✅ Works for both protocol and raw commands
✅ Accurate burst interval measurement
✅ Handles variable burst counts (2-20+ bursts)

### Debugging
✅ Serial Monitor shows detailed burst capture
✅ MQTT state updates provide real-time feedback
✅ Easy to verify burst patterns via JSON

## Technical Details

### Timing Constants

Defined in firmware (`ir_blaster.ino:66-69`):
```cpp
#define LEARNING_TOTAL_TIMEOUT_MS 10000  // Max 10s total learning time
#define BURST_IDLE_TIMEOUT_MS 500        // End learning if no signal for 500ms
#define MIN_BURST_INTERVAL_MS 50         // Min valid interval between bursts
#define MAX_BURST_INTERVAL_MS 500        // Max valid interval between bursts
```

### Burst Detection Logic

**Signal matching (`signalsMatch()` at line 618):**
- Protocol commands: Compare protocol + address + command
- Raw commands: Compare raw data length

**Burst capture (`handleLearnWindow()` at line 637):**
```cpp
// First signal
if (!hasBaseSignal) {
  baseSignal = IrReceiver.decodedIRData;  // Store as reference
  lastSignalTime = millis();
  capturedRepeats = 0;
}
// Subsequent signals within 500ms
else if (signalsMatch(baseSignal, currentSignal)) {
  capturedRepeats++;
  interval = millis() - lastSignalTime;
  lastSignalTime = millis();
}
```

**Learning termination:**
- 500ms with no new signal → End learning, save command
- 10 seconds total time → Force end learning
- Different signal detected → Ignored (user error)

### Interval Calculation

Average of all measured intervals:
```cpp
avgInterval = (lastRepeatTime - firstPressTime) / capturedRepeats;
```

This smooths out minor timing variations between bursts.

## Troubleshooting

### Not Detecting Bursts (Always Single)

**Problem:** Remote sends bursts but ESP32 only captures one

**Possible Causes:**
- IR receiver not sensitive enough (try closer range)
- Bursts spaced >500ms apart (increase `BURST_IDLE_TIMEOUT_MS`)
- Remote uses different codes per burst (check Serial Monitor)

**Solutions:**
- Point remote directly at receiver during learning
- Check Serial Monitor for "Burst #2" messages
- Verify remote is multi-burst with another IR receiver/camera

### Detecting False Bursts

**Problem:** Captures multiple bursts when remote only sends one

**Possible Causes:**
- IR reflections from nearby surfaces
- Button pressed multiple times accidentally
- Remote malfunction

**Solutions:**
- Learn in area without reflective surfaces
- Press button cleanly and release
- Manually override: set `repeatCount: 0` in MQTT message

### Wrong Burst Count

**Problem:** Captured count doesn't match remote's actual pattern

**Solutions:**
- Re-learn command (timing variations can affect detection)
- Manually override burst count via MQTT
- Check remote batteries (weak batteries cause irregular bursts)

### Command Not Working After Learning

**Problem:** Learned multi-burst command doesn't control device

**Solutions:**
1. Check device is in range and powered on
2. Verify burst count in Serial Monitor matches remote
3. Test with original remote to confirm device works
4. Try manually adjusting `repeatInterval` (±20ms)

## Advanced: Manual Override

You can manually edit commands to adjust burst behavior:

### Change Burst Count
```bash
mosquitto_pub -h homeassistant.local -u matt -P <pass> \
  -t 'home/ir/1/commands/tv_vol_up' \
  -m '{"proto":"Samsung","addr":7,"cmd":7,"rpt":0,"repeatCount":8,"repeatInterval":100}' \
  -r
```

### Change Burst Interval
```bash
mosquitto_pub -h homeassistant.local -u matt -P <pass> \
  -t 'home/ir/1/commands/fan_power' \
  -m '{"proto":"NEC","addr":0,"cmd":12,"rpt":0,"repeatCount":3,"repeatInterval":150}' \
  -r
```

### Convert Multi-Burst to Single-Burst
```bash
mosquitto_pub -h homeassistant.local -u matt -P <pass> \
  -t 'home/ir/1/commands/tv_vol_up' \
  -m '{"proto":"Samsung","addr":7,"cmd":7,"rpt":0,"repeatCount":0,"repeatInterval":0}' \
  -r
```

## Migration

Existing commands without repeat fields automatically default to:
- `repeatCount`: 0
- `repeatInterval`: 0

This ensures backward compatibility - all your existing single-burst commands work exactly as before.

## Code Changes

**Modified Files:**
- `ir_blaster.ino` - Lines 33-63 (struct), 116-184 (execution), 618-749 (learning/detection)

**New Features:**
- Automatic multi-burst detection during learning
- Burst pattern replay with precise timing
- MQTT status updates for burst detection
- Signal matching algorithm for burst identification
- Backward compatible with single-burst commands

**Memory Impact:**
- +4 bytes per command (repeatCount + repeatInterval)
- No heap allocation changes

## Common Remote Patterns

Based on testing, typical burst patterns:

| Device Type | Bursts | Interval | Notes |
|-------------|--------|----------|-------|
| Samsung TV | 1 or 6 | ~110ms | Power=1, Vol/Ch=6 |
| LG TV | 3 | ~100ms | Most buttons |
| Generic Fan | 4 | ~115ms | Speed/power controls |
| AC Units | 1 | N/A | Complex single-burst protocol |
| Cable Box | 1-2 | ~80ms | Varies by brand |

*Your remote may differ - Serial Monitor shows exact pattern*

## Future Enhancements (Optional)

Possible additions:
- Configurable idle timeout (currently fixed 500ms)
- Burst pattern validation (detect irregular patterns)
- Maximum burst limit (prevent runaway detection)
- Burst pattern database (pre-known remotes)

---

**Questions?** Check Serial Monitor output during learning for real-time burst detection information.
