# Home Assistant Configuration for MQTT IR Blaster

This document explains how to set up Home Assistant to work with the MQTT-based IR blaster.

## Overview

The IR blaster now stores all commands as MQTT retained messages instead of hardcoded in firmware. This allows you to:
- Add new commands without reflashing the ESP32
- Learn and save IR codes with simple names
- Manage commands entirely through Home Assistant

## MQTT Topics

| Topic | Direction | Purpose |
|-------|-----------|---------|
| `home/ir/1/send` | HA → ESP32 | Send IR command by name |
| `home/ir/1/listen` | HA → ESP32 | Start learning mode with command name |
| `home/ir/1/learn` | ESP32 → HA | Log of learned commands |
| `home/ir/1/state` | ESP32 → HA | Status updates |
| `home/ir/1/commands/*` | Bidirectional | Command definitions (retained) |

## Configuration Files

### 1. Input Text Helper (configuration.yaml)

Add this to your `configuration.yaml` or create via UI (Settings → Devices & Services → Helpers):

```yaml
input_text:
  ir_command_name:
    name: "New IR Command Name"
    max: 32
    icon: mdi:rename-box
    initial: ""
```

### 2. Scripts (scripts.yaml)

#### Learning Script

```yaml
ir_learn_command:
  alias: "Learn IR Command"
  description: "Start IR learning mode with a command name"
  sequence:
    - service: mqtt.publish
      data:
        topic: "home/ir/1/listen"
        payload: '{"name":"{{ states(''input_text.ir_command_name'') }}"}'
    - service: input_text.set_value
      target:
        entity_id: input_text.ir_command_name
      data:
        value: ""
    - service: notify.persistent_notification
      data:
        title: "IR Learning Started"
        message: "Point remote at sensor and press button within 10 seconds"
  icon: mdi:access-point
```

#### Send Command Scripts (Examples)

```yaml
# Generic script that takes command name as parameter
ir_send_command:
  alias: "Send IR Command"
  description: "Send any IR command by name"
  fields:
    command:
      description: "Command name to send"
      example: "tv_power"
  sequence:
    - service: mqtt.publish
      data:
        topic: "home/ir/1/send"
        payload: "{{ command }}"
  icon: mdi:remote

# Specific command scripts (for convenience)
ir_tv_power:
  alias: "TV Power"
  sequence:
    - service: mqtt.publish
      data:
        topic: "home/ir/1/send"
        payload: "tv_power"
  icon: mdi:television

ir_tv_vol_up:
  alias: "TV Volume Up"
  sequence:
    - service: mqtt.publish
      data:
        topic: "home/ir/1/send"
        payload: "tv_vol_up"
  icon: mdi:volume-plus

ir_tv_vol_down:
  alias: "TV Volume Down"
  sequence:
    - service: mqtt.publish
      data:
        topic: "home/ir/1/send"
        payload: "tv_vol_down"
  icon: mdi:volume-minus

ir_fan_power:
  alias: "Fan Power"
  sequence:
    - service: mqtt.publish
      data:
        topic: "home/ir/1/send"
        payload: "fan_power"
  icon: mdi:fan
```

### 3. Automations (automations.yaml)

#### Learning Success Notification

```yaml
- id: ir_command_learned
  alias: "IR Command Learned Notification"
  description: "Notify when IR command is successfully learned"
  trigger:
    - platform: mqtt
      topic: "home/ir/1/learn"
  action:
    - service: notify.persistent_notification
      data:
        title: "IR Command Learned"
        message: >
          Command '{{ trigger.payload_json.name }}' saved successfully!
          {% if trigger.payload_json.proto %}
          Protocol: {{ trigger.payload_json.proto }}
          Address: {{ trigger.payload_json.addr }}
          Command: {{ trigger.payload_json.cmd }}
          {% else %}
          Type: Raw timing data
          {% endif %}
```

#### Learning Timeout Alert

```yaml
- id: ir_learning_timeout
  alias: "IR Learning Timeout Alert"
  description: "Alert when learning mode times out"
  trigger:
    - platform: mqtt
      topic: "home/ir/1/state"
  condition:
    - condition: template
      value_template: "{{ 'learn_timeout' in trigger.payload }}"
  action:
    - service: notify.persistent_notification
      data:
        title: "IR Learning Timeout"
        message: "No IR signal received within 10 seconds. Try again."
```

## Lovelace Dashboard Card

Add this to your dashboard for easy command management:

```yaml
type: vertical-stack
cards:
  # Learning Section
  - type: entities
    title: Learn New IR Command
    entities:
      - entity: input_text.ir_command_name
        name: Command Name
      - type: button
        name: Start Learning (10s)
        icon: mdi:access-point
        tap_action:
          action: call-service
          service: script.ir_learn_command
    show_header_toggle: false

  # TV Controls
  - type: entities
    title: TV Controls
    entities:
      - type: button
        name: Power
        icon: mdi:power
        tap_action:
          action: call-service
          service: script.ir_tv_power
      - type: button
        name: Volume Up
        icon: mdi:volume-plus
        tap_action:
          action: call-service
          service: script.ir_tv_vol_up
      - type: button
        name: Volume Down
        icon: mdi:volume-minus
        tap_action:
          action: call-service
          service: script.ir_tv_vol_down
    show_header_toggle: false

  # Fan Controls
  - type: entities
    title: Fan Controls
    entities:
      - type: button
        name: Power
        icon: mdi:power
        tap_action:
          action: call-service
          service: script.ir_fan_power
      - type: button
        name: Speed Up
        icon: mdi:fan-plus
        tap_action:
          action: call-service
          service: script.ir_send_command
          service_data:
            command: fan_speed_up
      - type: button
        name: Speed Down
        icon: mdi:fan-minus
        tap_action:
          action: call-service
          service: script.ir_send_command
          service_data:
            command: fan_speed_down
    show_header_toggle: false
```

## Usage Workflows

### Adding a New Command via Learning Mode

1. **Enter command name** in the "New IR Command Name" input field (e.g., `bedroom_tv_input`)
2. **Click "Start Learning"** button
3. **Point your remote** at the IR receiver on the ESP32
4. **Press the button** you want to learn within 10 seconds
5. **Check notification** for success message
6. **Test the command** by creating a script or button that sends the command name

The command is now permanently stored in the MQTT broker and the ESP32 cache.

### Adding a Command Manually (Advanced)

You can manually publish commands to MQTT using `mosquitto_pub` or MQTT Explorer:

**Protocol Command (e.g., NEC):**
```bash
mosquitto_pub -h homeassistant.local -u matt -P <password> \
  -t 'home/ir/1/commands/living_room_tv_power' \
  -m '{"proto":"NEC","addr":34,"cmd":12,"rpt":0}' \
  -r
```

**Raw Command:**
```bash
mosquitto_pub -h homeassistant.local -u matt -P <password> \
  -t 'home/ir/1/commands/custom_device' \
  -m '{"raw":true,"freq":38,"data":[1000,500,1000,500,...]}' \
  -r
```

**Important:** Use the `-r` flag to make the message **retained**.

### Deleting a Command

Publish an empty retained message to the command's topic:

```bash
mosquitto_pub -h homeassistant.local -u matt -P <password> \
  -t 'home/ir/1/commands/unwanted_command' \
  -n -r
```

The ESP32 will remove it from cache on the next message.

### Viewing All Commands

```bash
mosquitto_sub -h homeassistant.local -u matt -P <password> \
  -t 'home/ir/1/commands/#' -v
```

## Command Name Guidelines

- Use lowercase with underscores: `bedroom_tv_power`
- Be descriptive: `living_room_fan_speed_3` instead of `fan3`
- Max 31 characters
- Alphanumeric and underscores only

## Supported Protocols

### Protocol Commands
- Samsung
- NEC
- LG
- Sony12
- JVC
- RC5
- RC6
- Panasonic

### Raw Commands
For unsupported or proprietary protocols, the ESP32 will automatically capture raw timing data.

## Troubleshooting

### Commands Not Loading on ESP32 Boot

Check that:
1. MQTT broker is running and accessible
2. Commands are published with `retain=true` flag
3. ESP32 is subscribed to `home/ir/1/commands/#`
4. Serial monitor shows "Loaded X commands from MQTT"

### Learning Mode Not Starting

Check:
1. MQTT connection is active (check `home/ir/1/state` topic)
2. Command name is not empty
3. Not already in learning mode

### Command Not Executing

Check:
1. Command exists in cache (ESP32 Serial: "Loaded X commands")
2. Command name matches exactly (case-sensitive)
3. `home/ir/1/state` topic for error messages

## Advanced: Node-RED Auto-Save (Optional)

For automatic command storage in Home Assistant, you can use Node-RED:

1. Listen to `home/ir/1/learn` topic
2. Parse JSON payload
3. Write to `scripts.yaml` or update input_text helpers
4. Reload Home Assistant scripts

Example flow available upon request.

## Backup & Restore

### Backup Commands

```bash
mosquitto_sub -h homeassistant.local -u matt -P <password> \
  -t 'home/ir/1/commands/#' -v > ir_commands_backup.txt
```

### Restore Commands

Parse the backup file and republish each command with `retain=true`.

## Migration Notes

Your existing 8 commands have been migrated to MQTT using the `migrate_commands.py` script. They are now stored as:

- `home/ir/1/commands/tv_power`
- `home/ir/1/commands/tv_vol_up`
- `home/ir/1/commands/tv_vol_down`
- `home/ir/1/commands/tv_mute`
- `home/ir/1/commands/fan_power`
- `home/ir/1/commands/fan_speed_up`
- `home/ir/1/commands/fan_speed_down`
- `home/ir/1/commands/fan_rotate`

These will automatically load into the ESP32 on boot.
