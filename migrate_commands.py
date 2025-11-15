#!/usr/bin/env python3
"""
MQTT IR Command Migration Script

This script publishes the hardcoded IR commands to MQTT as retained messages,
allowing the ESP32 to load them from the broker instead of having them in firmware.

Requirements:
  pip install paho-mqtt

Usage:
  python3 migrate_commands.py
"""

import paho.mqtt.client as mqtt
import json
import time

# MQTT Broker Configuration
MQTT_HOST = "homeassistant.local"
MQTT_PORT = 1883
MQTT_USER = "matt"
MQTT_PASS = "W8j!&VQ3*sXUnWIEx!$z6"

# Base topic for commands
COMMAND_TOPIC_BASE = "home/ir/1/commands/"

# Define all commands to migrate
COMMANDS = {
    # TV Commands (Samsung protocol)
    "tv_power": {
        "proto": "Samsung",
        "addr": 7,
        "cmd": 2,
        "rpt": 0
    },
    "tv_vol_up": {
        "proto": "Samsung",
        "addr": 7,
        "cmd": 7,
        "rpt": 0
    },
    "tv_vol_down": {
        "proto": "Samsung",
        "addr": 7,
        "cmd": 11,
        "rpt": 0
    },
    "tv_mute": {
        "proto": "Samsung",
        "addr": 7,
        "cmd": 15,
        "rpt": 0
    },

    # Fan Commands (Raw timing data)
    "fan_power": {
        "raw": True,
        "freq": 38,
        "data": [1330,270,1380,270,580,1220,1280,270,1430,320,480,1220,430,1220,480,1220,430,1220,430,1220,430,1220,1330,7070,1280,370,1330,270,530,1220,1330,220,1430,270,580,1220,480,1170,480,1170,480,1170,480,1220,430,1220,1330,8020,1330,320,1330,370,480,1220,1280,370,1330,320,480,1220,480,1170,430,1220,430,1270,430,1220,430,1220,1280,7120,1280,370,1280,420,430,1220,1280,420,1280,370,430,1270,380,1270,430,1220,430,1270,380,1270,380,1270,1230]
    },
    "fan_speed_up": {
        "raw": True,
        "freq": 38,
        "data": [1180,2420,230,1320,180,770,230,420,230,120,180,120,380,170,180,220,280,1470,180,1470,280,1370,230,1420,330,1320,1180,570,280,170,180,7520,1180,520,1180,520,230,1420,1180,520,1180,570,180,1420,280,1370,230,1370,380,1320,280,120,230,1020,1180,570,280]
    },
    "fan_speed_down": {
        "raw": True,
        "freq": 38,
        "data": [1280,370,1330,370,430,1220,1280,320,1380,320,530,1220,430,1220,1230,420,430,1270,380,1270,1280,320,530,7870,1280,320,1380,370,430,1270,1230,370,1330,370,480,1220,430,1220,1280,370,480,1220,430,1220,1280,320,530]
    },
    "fan_rotate": {
        "raw": True,
        "freq": 38,
        "data": [1230,120,1580,420,380,1270,230,120,880,470,230,120,880,420,130,120,180,1220,230,1470,230,120,280,120,130,120,230,470,230,1370,230,1470,230,1420,230]
    }
}


def on_connect(client, userdata, flags, rc):
    if rc == 0:
        print("✓ Connected to MQTT broker")
    else:
        print(f"✗ Connection failed with code {rc}")


def publish_commands(client):
    """Publish all commands as retained MQTT messages"""

    print(f"\nPublishing {len(COMMANDS)} commands to MQTT broker...")
    print(f"Broker: {MQTT_HOST}:{MQTT_PORT}\n")

    success_count = 0

    for name, command_data in COMMANDS.items():
        topic = COMMAND_TOPIC_BASE + name
        payload = json.dumps(command_data)

        # Publish as retained message
        result = client.publish(topic, payload, qos=1, retain=True)

        if result.rc == mqtt.MQTT_ERR_SUCCESS:
            print(f"✓ Published: {name}")
            if "proto" in command_data:
                print(f"    Type: Protocol ({command_data['proto']})")
            else:
                print(f"    Type: Raw ({len(command_data['data'])} values)")
            success_count += 1
        else:
            print(f"✗ Failed: {name}")

    print(f"\n{success_count}/{len(COMMANDS)} commands published successfully")
    print("\nThese commands are now stored as retained messages on the MQTT broker.")
    print("The ESP32 will automatically load them on boot/reconnect.")


def main():
    print("=" * 60)
    print("IR Command Migration to MQTT")
    print("=" * 60)

    # Create MQTT client
    client = mqtt.Client(client_id="ir_migration_script")
    client.username_pw_set(MQTT_USER, MQTT_PASS)
    client.on_connect = on_connect

    try:
        # Connect to broker
        print(f"\nConnecting to {MQTT_HOST}:{MQTT_PORT}...")
        client.connect(MQTT_HOST, MQTT_PORT, 60)
        client.loop_start()

        # Wait for connection
        time.sleep(1)

        # Publish all commands
        publish_commands(client)

        # Wait for messages to be sent
        time.sleep(1)

        client.loop_stop()
        client.disconnect()

        print("\n" + "=" * 60)
        print("Migration complete!")
        print("=" * 60)
        print("\nNext steps:")
        print("1. Flash the updated firmware to your ESP32")
        print("2. Watch the Serial monitor - should see 'Loaded 8 commands from MQTT'")
        print("3. Test commands by publishing to home/ir/1/send")
        print("   Example: mosquitto_pub -h homeassistant.local -u matt -P <pass> \\")
        print("            -t 'home/ir/1/send' -m 'tv_power'")
        print("\nTo view all commands:")
        print("  mosquitto_sub -h homeassistant.local -u matt -P <pass> \\")
        print("                -t 'home/ir/1/commands/#' -v")

    except Exception as e:
        print(f"\n✗ Error: {e}")
        return 1

    return 0


if __name__ == "__main__":
    exit(main())
