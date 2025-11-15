#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <IRremote.hpp>
#include <ArduinoJson.h>

// ====== WiFi/MQTT Configuration ======
// Credentials are stored in credentials.h (not tracked in git)
// Copy credentials.h.example to credentials.h and update with your values
#include "credentials.h"

// Topics
#define TOPIC_IR_SEND  "home/ir/1/send"        // HA -> ESP (send command by name)
#define TOPIC_STATE    "home/ir/1/state"       // ESP -> HA (status updates)
#define TOPIC_LEARN    "home/ir/1/learn"       // ESP -> HA (learned command log)
#define TOPIC_LISTEN   "home/ir/1/listen"      // HA -> ESP (begin 10s listening with name)
#define TOPIC_COMMANDS "home/ir/1/commands/#"  // HA -> ESP (command definitions, retained)


WiFiClient espClient;
PubSubClient mqtt(espClient);

const uint8_t ONBOARD_LED = 2;

// ====== Command Cache ======
#define MAX_COMMANDS 30
#define MAX_COMMAND_NAME 32
#define MAX_RAW_DATA 200  // Max raw timing values per command

struct StoredCommand {
  char name[MAX_COMMAND_NAME];
  bool isRaw;
  uint8_t repeatCount;        // Number of repeats captured (0 = single press)
  uint16_t repeatInterval;    // Milliseconds between repeats
  union {
    struct {
      char proto[16];    // Protocol name as string
      uint16_t addr;
      uint16_t cmd;
      uint8_t rpt;       // Protocol-level repeats (always 0, bursts handled by repeatCount)
    } protocol;
    struct {
      uint8_t freq;
      uint16_t data[MAX_RAW_DATA];
      uint16_t len;
    } raw;
  };
};

StoredCommand commandCache[MAX_COMMANDS];
uint8_t commandCount = 0;
char learningCommandName[MAX_COMMAND_NAME] = "";

// Burst capture tracking
static uint8_t capturedRepeats = 0;
static uint32_t firstPressTime = 0;
static uint32_t lastRepeatTime = 0;
static uint32_t lastSignalTime = 0;
static IRData baseSignal;  // Store first signal for comparison
static bool hasBaseSignal = false;

// Learning mode timing
#define LEARNING_TOTAL_TIMEOUT_MS 10000  // Maximum 10s total learning time
#define BURST_IDLE_TIMEOUT_MS 500        // End learning if no signal for 500ms

// ====== Input ======
// Button-based learning removed - now using MQTT TOPIC_LISTEN
// wiring: pin ---[10k]-> GND, and pin ---switch--- 5V
// const uint8_t INPUT_BUTTON_PIN = 26;   // pick your GPIO
// const uint16_t DEBOUNCE_MS               = 25;
// const uint16_t INTER_PRESS_MAX_MS        = 350;
// const uint16_t SEQUENCE_TOTAL_TIMEOUT_MS = 1500;
// const uint8_t  REQUIRED_PRESSES          = 5;

static bool     learnActive   = false;
static uint32_t learnDeadline = 0;

// ====== IR ======
constexpr uint8_t IR_SEND_PIN = 13;
constexpr uint8_t IR_RECEIVE_PIN = 27;


enum class Proto : uint8_t { Samsung, NEC, LG, Sony12, JVC, RC5, RC6, Panasonic };

// ====== Command Cache Management ======

// Find command by name in cache
StoredCommand* findCommandByName(const char* name) {
  for (uint8_t i = 0; i < commandCount; i++) {
    if (strcmp(commandCache[i].name, name) == 0) {
      return &commandCache[i];
    }
  }
  return nullptr;
}

// Forward declaration
void indicateSend();

// Parse protocol string to Proto enum
Proto parseProto(const char* protoStr) {
  if (strcasecmp(protoStr, "Samsung") == 0) return Proto::Samsung;
  if (strcasecmp(protoStr, "NEC") == 0) return Proto::NEC;
  if (strcasecmp(protoStr, "LG") == 0) return Proto::LG;
  if (strcasecmp(protoStr, "Sony12") == 0) return Proto::Sony12;
  if (strcasecmp(protoStr, "JVC") == 0) return Proto::JVC;
  if (strcasecmp(protoStr, "RC5") == 0) return Proto::RC5;
  if (strcasecmp(protoStr, "RC6") == 0) return Proto::RC6;
  if (strcasecmp(protoStr, "Panasonic") == 0) return Proto::Panasonic;
  return Proto::NEC;  // default
}

// Execute a cached command
void executeCommand(StoredCommand* cmd) {
  if (!cmd) {
    Serial.println("ERROR: Null command pointer");
    mqtt.publish(TOPIC_STATE, "ERR:NULL_COMMAND");
    return;
  }

  Serial.print("Executing command: ");
  Serial.println(cmd->name);

  // Calculate total send count (initial + repeats)
  uint8_t sendCount = 1 + cmd->repeatCount;

  if (cmd->repeatCount > 0) {
    Serial.print("Will send ");
    Serial.print(sendCount);
    Serial.print(" times with ");
    Serial.print(cmd->repeatInterval);
    Serial.println("ms interval");
  }

  // Send command (initial + repeats)
  for (uint8_t i = 0; i < sendCount; i++) {
    if (i > 0) {
      // Delay before sending next burst
      delay(cmd->repeatInterval);
      Serial.print("Sending burst #");
      Serial.println(i);
    }

    if (cmd->isRaw) {
      // Send raw IR data
      if (i == 0) {
        Serial.print("Sending raw command, freq=");
        Serial.print(cmd->raw.freq);
        Serial.print(", len=");
        Serial.println(cmd->raw.len);
      }
      indicateSend();
      IrSender.sendRaw(cmd->raw.data, cmd->raw.len, cmd->raw.freq);
    } else {
      // Send protocol command
      if (i == 0) {
        Serial.print("Sending protocol command: ");
        Serial.println(cmd->protocol.proto);
      }

      Proto proto = parseProto(cmd->protocol.proto);
      uint16_t addr = cmd->protocol.addr;
      uint8_t command = (uint8_t)cmd->protocol.cmd;
      // Protocol-level repeats (always 0) - handled at burst level via repeatCount instead
      uint8_t repeats = cmd->protocol.rpt;

      switch (proto) {
        case Proto::Samsung:   IrSender.sendSamsung(addr, command, repeats); break;
        case Proto::NEC:       IrSender.sendNEC(addr, command, repeats); break;
        case Proto::LG:        IrSender.sendLG(addr, command, repeats); break;
        case Proto::Sony12:    IrSender.sendSony(addr, command, 12, repeats); break;
        case Proto::RC5:       IrSender.sendRC5(addr, command, 0, repeats); break;
        case Proto::RC6:       IrSender.sendRC6(addr, command, 20, repeats); break;
        case Proto::Panasonic: IrSender.sendPanasonic(addr, cmd->protocol.cmd & 0x0FFF, repeats); break;
      }
    }
  }

  char msg[64];
  snprintf(msg, sizeof(msg), "OK:%s", cmd->name);
  mqtt.publish(TOPIC_STATE, msg);
  Serial.println("Command sent successfully");
}

// blink onboard led to indicate sending
void indicateSend() {
  for (int i = 0; i < 3; i++) {
    digitalWrite(ONBOARD_LED, HIGH);
    delay(200);
    digitalWrite(ONBOARD_LED, LOW);
    delay(200);
  }
}

// Add or update command in cache
bool addOrUpdateCommand(const char* name, JsonDocument& doc) {
  if (strlen(name) >= MAX_COMMAND_NAME) {
    Serial.println("ERROR: Command name too long");
    return false;
  }

  // Check if command already exists
  StoredCommand* existing = findCommandByName(name);
  StoredCommand* cmd;

  if (existing) {
    Serial.print("Updating existing command: ");
    Serial.println(name);
    cmd = existing;
  } else {
    if (commandCount >= MAX_COMMANDS) {
      Serial.println("ERROR: Command cache full");
      mqtt.publish(TOPIC_STATE, "ERR:CACHE_FULL");
      return false;
    }
    Serial.print("Adding new command: ");
    Serial.println(name);
    cmd = &commandCache[commandCount++];
  }

  // Copy name
  strncpy(cmd->name, name, MAX_COMMAND_NAME - 1);
  cmd->name[MAX_COMMAND_NAME - 1] = '\0';

  // Parse repeat fields (default to 0 if not present for backward compatibility)
  cmd->repeatCount = doc["repeatCount"] | 0;
  cmd->repeatInterval = doc["repeatInterval"] | 0;

  if (cmd->repeatCount > 0) {
    Serial.print("  Repeat info: count=");
    Serial.print(cmd->repeatCount);
    Serial.print(", interval=");
    Serial.print(cmd->repeatInterval);
    Serial.println("ms");
  }

  // Check if raw or protocol command
  if (doc["raw"].is<bool>() && doc["raw"]) {
    // Raw command
    cmd->isRaw = true;
    cmd->raw.freq = doc["freq"] | 38;  // default 38kHz

    JsonArray dataArray = doc["data"];
    cmd->raw.len = min((int)dataArray.size(), MAX_RAW_DATA);

    for (uint16_t i = 0; i < cmd->raw.len; i++) {
      cmd->raw.data[i] = dataArray[i];
    }

    Serial.print("  Raw command: freq=");
    Serial.print(cmd->raw.freq);
    Serial.print(", len=");
    Serial.println(cmd->raw.len);
  } else {
    // Protocol command
    cmd->isRaw = false;

    const char* proto = doc["proto"] | "NEC";
    strncpy(cmd->protocol.proto, proto, 15);
    cmd->protocol.proto[15] = '\0';

    cmd->protocol.addr = doc["addr"] | 0;
    cmd->protocol.cmd = doc["cmd"] | 0;
    cmd->protocol.rpt = doc["rpt"] | 0;

    Serial.print("  Protocol command: ");
    Serial.print(cmd->protocol.proto);
    Serial.print(", addr=");
    Serial.print(cmd->protocol.addr);
    Serial.print(", cmd=");
    Serial.println(cmd->protocol.cmd);
  }

  return true;
}

// Delete command from cache
bool deleteCommand(const char* name) {
  for (uint8_t i = 0; i < commandCount; i++) {
    if (strcmp(commandCache[i].name, name) == 0) {
      Serial.print("Deleting command: ");
      Serial.println(name);

      // Shift remaining commands down
      for (uint8_t j = i; j < commandCount - 1; j++) {
        commandCache[j] = commandCache[j + 1];
      }
      commandCount--;
      return true;
    }
  }
  return false;
}

// ====== REMOVED: Hardcoded Commands ======
// All commands now stored as MQTT retained messages on broker.
// See migration script to publish these to MQTT.
//
// OLD TV COMMANDS (Samsung, addr=7):
//   tv_power    -> cmd=2
//   tv_vol_up   -> cmd=7
//   tv_vol_down -> cmd=11
//   tv_mute     -> cmd=15
//
// OLD FAN RAW COMMANDS:
//   fan_power        -> 95 values: 1330,270,1380,270,580,1220,1280,270,1430,320,480,1220,430,1220,480,1220,430,1220,430,1220,430,1220,1330,7070,1280,370,1330,270,530,1220,1330,220,1430,270,580,1220,480,1170,480,1170,480,1170,480,1220,430,1220,1330,8020,1330,320,1330,370,480,1220,1280,370,1330,320,480,1220,480,1170,430,1220,430,1270,430,1220,430,1220,1280,7120,1280,370,1280,420,430,1220,1280,420,1280,370,430,1270,380,1270,430,1220,430,1270,380,1270,380,1270,1230
//   fan_speed_up     -> 57 values: 1180,2420,230,1320,180,770,230,420,230,120,180,120,380,170,180,220,280,1470,180,1470,280,1370,230,1420,330,1320,1180,570,280,170,180,7520,1180,520,1180,520,230,1420,1180,520,1180,570,180,1420,280,1370,230,1370,380,1320,280,120,230,1020,1180,570,280
//   fan_speed_down   -> 47 values: 1280,370,1330,370,430,1220,1280,320,1380,320,530,1220,430,1220,1230,420,430,1270,380,1270,1280,320,530,7870,1280,320,1380,370,430,1270,1230,370,1330,370,480,1220,430,1220,1280,370,480,1220,430,1220,1280,320,530
//   fan_rotate       -> 35 values: 1230,120,1580,420,380,1270,230,120,880,470,230,120,880,420,130,120,180,1220,230,1470,230,120,280,120,130,120,230,470,230,1370,230,1470,230,1420,230
//
// Run the migration Python script to publish these to MQTT broker as retained messages.

// MQTT Message Handler with Topic Routing
void onMqttMessage(char* topic, byte* payload, unsigned int len) {
  Serial.print("MQTT message on topic: ");
  Serial.println(topic);

  // Copy payload to buffer (with larger size for JSON)
  static char buf[1024];
  len = min((unsigned)sizeof(buf)-1, len);
  memcpy(buf, payload, len);
  buf[len] = '\0';

  // Route based on topic

  // ===== TOPIC_LISTEN: Trigger learning mode with command name =====
  if (strcmp(topic, TOPIC_LISTEN) == 0) {
    if (learnActive) {
      Serial.println("Already in learn mode");
      return;
    }

    // Parse JSON to get command name
    StaticJsonDocument<512> doc;  // Enough for listen command JSON
    DeserializationError error = deserializeJson(doc, buf);

    if (error) {
      Serial.print("JSON parse error: ");
      Serial.println(error.c_str());
      mqtt.publish(TOPIC_STATE, "ERR:INVALID_JSON");
      return;
    }

    const char* name = doc["name"];
    if (!name || strlen(name) == 0) {
      Serial.println("No command name provided");
      mqtt.publish(TOPIC_STATE, "ERR:NO_NAME");
      return;
    }

    if (strlen(name) >= MAX_COMMAND_NAME) {
      Serial.print("Command name too long (max ");
      Serial.print(MAX_COMMAND_NAME - 1);
      Serial.println(" chars)");
      mqtt.publish(TOPIC_STATE, "ERR:NAME_TOO_LONG");
      return;
    }

    // Store name for learning
    strncpy(learningCommandName, name, MAX_COMMAND_NAME - 1);
    learningCommandName[MAX_COMMAND_NAME - 1] = '\0';

    // Start learning mode
    learnActive = true;
    learnDeadline = millis() + 10000UL;  // 10s window
    IrReceiver.begin(IR_RECEIVE_PIN, DISABLE_LED_FEEDBACK);

    char msg[96];
    snprintf(msg, sizeof(msg), "learn_start:%s", learningCommandName);
    mqtt.publish(TOPIC_STATE, msg);

    Serial.print("Learn mode started for: ");
    Serial.println(learningCommandName);
    return;
  }

  // ===== TOPIC_IR_SEND: Send command by name =====
  if (strcmp(topic, TOPIC_IR_SEND) == 0) {
    // Simple command name in payload
    if (len == 0 || buf[0] == '\0') {
      Serial.println("Empty command name in send request");
      mqtt.publish(TOPIC_STATE, "ERR:EMPTY_COMMAND_NAME");
      return;
    }

    StoredCommand* cmd = findCommandByName(buf);
    if (!cmd) {
      Serial.print("Command not found: ");
      Serial.println(buf);
      char msg[96];
      snprintf(msg, sizeof(msg), "ERR:NOT_FOUND:%s", buf);
      mqtt.publish(TOPIC_STATE, msg);
      return;
    }

    executeCommand(cmd);
    return;
  }

  // ===== TOPIC_COMMANDS/*: Command definition (add/update/delete) =====
  if (strncmp(topic, "home/ir/1/commands/", 19) == 0) {
    // Extract command name from topic
    const char* commandName = topic + 19;  // Skip "home/ir/1/commands/"

    // Empty payload = delete command
    if (len == 0) {
      if (deleteCommand(commandName)) {
        Serial.print("Deleted command: ");
        Serial.println(commandName);
        char msg[96];
        snprintf(msg, sizeof(msg), "deleted:%s", commandName);
        mqtt.publish(TOPIC_STATE, msg);
      }
      return;
    }

    // Parse JSON command definition
    StaticJsonDocument<2048> doc;  // Large enough for MAX_RAW_DATA (200 values)
    DeserializationError error = deserializeJson(doc, buf);

    if (error) {
      Serial.print("JSON parse error: ");
      Serial.println(error.c_str());
      char msg[96];
      snprintf(msg, sizeof(msg), "ERR:JSON:%s", commandName);
      mqtt.publish(TOPIC_STATE, msg);
      return;
    }

    // Add or update command
    if (addOrUpdateCommand(commandName, doc)) {
      char msg[96];
      snprintf(msg, sizeof(msg), "cached:%s", commandName);
      mqtt.publish(TOPIC_STATE, msg);
    }
    return;
  }
}

void ensureMqtt() {
  while (!mqtt.connected()) {
    if (mqtt.connect(MQTT_CLIENTID, MQTT_USER, MQTT_PASS, TOPIC_STATE, 0, true, "offline")) {
      Serial.println("MQTT connected!");

      // Subscribe to command topics
      mqtt.subscribe(TOPIC_IR_SEND);
      mqtt.subscribe(TOPIC_LISTEN);
      mqtt.subscribe(TOPIC_COMMANDS);  // Receives all retained command definitions
      Serial.println("Subscribed to topics");

      // Wait briefly for retained messages to arrive
      delay(500);
      mqtt.loop();  // Process incoming retained messages

      // Publish status
      char msg[64];
      snprintf(msg, sizeof(msg), "online (loaded %d commands)", commandCount);
      mqtt.publish(TOPIC_STATE, msg);
      Serial.print("Loaded ");
      Serial.print(commandCount);
      Serial.println(" commands from MQTT");
    } else {
      Serial.print("MQTT connection failed, rc=");
      Serial.println(mqtt.state());
      delay(1000);
    }
  }
}


// Button-based learning removed - now triggered via MQTT on TOPIC_LISTEN
// bool listenForReadEnable() {
//   static int      lastRaw        = !HIGH;
//   static int      stableState    = !HIGH;
//   static uint32_t lastBounceTime = 0;
//
//   static uint8_t  pressCount     = 0;
//   static uint32_t firstPressTime = 0;
//   static uint32_t lastPressTime  = 0;
//
//   const uint32_t now = millis();
//   const int raw = digitalRead(INPUT_BUTTON_PIN);
//
//   if (raw != lastRaw) {
//     lastRaw = raw;
//     lastBounceTime = now;
//   }
//
//   if ((now - lastBounceTime) > DEBOUNCE_MS && raw != stableState) {
//     stableState = raw;
//
//     if (stableState == HIGH) {
//       Serial.println("Button pressed (debounced)");
//       if (pressCount == 0) {
//         pressCount = 1; firstPressTime = now; lastPressTime = now;
//       } else if ((now - lastPressTime) <= INTER_PRESS_MAX_MS &&
//                  (now - firstPressTime) <= SEQUENCE_TOTAL_TIMEOUT_MS) {
//         pressCount++; lastPressTime = now;
//         Serial.print("Press count: ");
//         Serial.println(pressCount);
//       } else {
//         pressCount = 1; firstPressTime = now; lastPressTime = now;
//       }
//
//       if (pressCount >= REQUIRED_PRESSES) {
//         pressCount = 0;
//         Serial.println("5 presses detected! Entering learn mode.");
//         return true;
//       }
//     }
//   }
//
//   if (pressCount > 0 &&
//      ((now - lastPressTime) > INTER_PRESS_MAX_MS ||
//       (now - firstPressTime) > SEQUENCE_TOTAL_TIMEOUT_MS)) {
//     pressCount = 0;
//   }
//
//   return false;
// }

// Publish learned command as retained message
static void publishDecode() {
  const IRData &d = baseSignal;  // Use base signal, not current decodedIRData
  char topic[96];
  char msg[2048];

  if (strlen(learningCommandName) == 0) {
    Serial.println("ERROR: No command name set for learning");
    return;
  }

  // Calculate average repeat interval
  uint16_t avgInterval = 0;
  if (capturedRepeats > 0) {
    uint32_t totalTime = lastRepeatTime - firstPressTime;
    avgInterval = totalTime / capturedRepeats;
  }

  // Build topic for command storage
  snprintf(topic, sizeof(topic), "home/ir/1/commands/%s", learningCommandName);

  if (d.protocol != UNKNOWN) {
    // ===== Known Protocol Command =====
    Serial.println("Known protocol detected");

    // Build JSON for protocol command with repeat info
    snprintf(msg, sizeof(msg),
      "{\"proto\":\"%s\",\"addr\":%lu,\"cmd\":%lu,\"rpt\":0,\"repeatCount\":%u,\"repeatInterval\":%u}",
      getProtocolString(d.protocol),
      (unsigned long)d.address,
      (unsigned long)d.command,
      capturedRepeats,
      avgInterval);

    // Publish as RETAINED command definition
    mqtt.publish(topic, msg, true);

    // Also publish to learn topic for logging (non-retained)
    char logMsg[256];
    snprintf(logMsg, sizeof(logMsg),
      "{\"name\":\"%s\",\"proto\":\"%s\",\"addr\":%lu,\"cmd\":%lu}",
      learningCommandName,
      getProtocolString(d.protocol),
      (unsigned long)d.address,
      (unsigned long)d.command);
    mqtt.publish(TOPIC_LEARN, logMsg, false);

    Serial.print("Published protocol command: ");
    Serial.println(learningCommandName);

  } else {
    // ===== Unknown Protocol - Use Raw Timing Data =====
    Serial.println("Unknown protocol - using raw data");

    // Build JSON with raw timing array
    // Format: {"raw":true,"freq":38,"data":[123,456,789,...]}

    strcpy(msg, "{\"raw\":true,\"freq\":38,\"data\":[");

    // Add timing values using IRremote 4.x API
    // Access raw buffer via decodedIRData.rawDataPtr
    for (uint16_t i = 1; i < d.rawlen; i++) {
      char num[8];
      // IRremote 4.x: rawbuf is accessible via rawDataPtr
      unsigned int timing = IrReceiver.decodedIRData.rawDataPtr->rawbuf[i] * MICROS_PER_TICK;

      // Check if we have room (leave 20 chars for closing)
      if (strlen(msg) + 20 > sizeof(msg)) {
        Serial.println("WARNING: Raw data too long, truncating");
        break;
      }

      snprintf(num, sizeof(num), "%u", timing);
      strcat(msg, num);

      if (i < d.rawlen - 1) {
        strcat(msg, ",");
      }
    }

    // Add repeat info to raw command JSON
    char repeatInfo[64];
    snprintf(repeatInfo, sizeof(repeatInfo), "],\"repeatCount\":%u,\"repeatInterval\":%u}", capturedRepeats, avgInterval);
    strcat(msg, repeatInfo);

    // Publish as RETAINED command definition
    mqtt.publish(topic, msg, true);

    // Also publish simpler log message
    char logMsg[128];
    snprintf(logMsg, sizeof(logMsg),
      "{\"name\":\"%s\",\"raw\":true,\"len\":%u}",
      learningCommandName,
      d.rawlen - 1);
    mqtt.publish(TOPIC_LEARN, logMsg, false);

    Serial.print("Published raw command: ");
    Serial.println(learningCommandName);

    // Print raw array to Serial for reference
    IrReceiver.compensateAndPrintIRResultAsCArray(&Serial, true);
    Serial.println();
  }

  Serial.print("Command saved to: ");
  Serial.println(topic);
}

// Compare two IR signals to see if they're identical
bool signalsMatch(const IRData& sig1, const IRData& sig2) {
  // Different protocols = different signals
  if (sig1.protocol != sig2.protocol) return false;

  // For known protocols, compare address and command
  if (sig1.protocol != UNKNOWN) {
    if (sig1.address != sig2.address) return false;
    if (sig1.command != sig2.command) return false;
    return true;
  }

  // For unknown/raw protocols, compare raw data length
  // (Full raw comparison would be too expensive, length is good enough)
  if (sig1.rawlen != sig2.rawlen) return false;

  return true;  // Same length raw data = probably same signal
}

// call from loop()
static void handleLearnWindow() {
  if (!learnActive) return;

  uint32_t now = millis();

  // Decode incoming IR signals
  if (IrReceiver.decode()) {
    // First signal - store as base for comparison
    if (!hasBaseSignal) {
      Serial.println("First signal captured, listening for bursts (500ms idle timeout)...");
      baseSignal = IrReceiver.decodedIRData;  // Store entire signal
      hasBaseSignal = true;
      firstPressTime = now;
      lastSignalTime = now;
      lastRepeatTime = now;
      capturedRepeats = 0;  // Will count additional bursts (0 = single send)

      // Set maximum timeout (10 seconds total)
      learnDeadline = now + LEARNING_TOTAL_TIMEOUT_MS;

      // Print to serial
      IrReceiver.printIRResultShort(&Serial);
      Serial.println();
    }
    // Subsequent signals - compare to base
    else {
      const IRData& currentSignal = IrReceiver.decodedIRData;

      // Check if this signal matches the base signal
      if (signalsMatch(baseSignal, currentSignal)) {
        capturedRepeats++;
        uint16_t interval = now - lastSignalTime;
        lastSignalTime = now;
        lastRepeatTime = now;

        Serial.print("Burst #");
        Serial.print(capturedRepeats + 1);  // +1 because we count additional bursts
        Serial.print(" detected (interval: ");
        Serial.print(interval);
        Serial.println("ms)");

        // Publish burst detection status
        char msg[64];
        snprintf(msg, sizeof(msg), "learn_burst_detected:%d", capturedRepeats + 1);
        mqtt.publish(TOPIC_STATE, msg);
      } else {
        Serial.println("Different signal detected, ignoring (press same button only)");
      }
    }

    IrReceiver.resume();  // Continue listening
    return;
  }

  // Check for idle timeout (500ms with no signal) OR max timeout (10s total)
  uint32_t timeSinceLastSignal = now - lastSignalTime;
  bool idleTimeout = hasBaseSignal && (timeSinceLastSignal > BURST_IDLE_TIMEOUT_MS);
  bool maxTimeout = now > learnDeadline;

  if (idleTimeout || maxTimeout) {
    if (!hasBaseSignal) {
      // No signal received at all
      Serial.println("Learning timeout - no signal received");
      mqtt.publish(TOPIC_STATE, "learn_timeout:no_signal");
    } else {
      // Got signal(s), end learning
      if (idleTimeout) {
        Serial.print("Burst sequence complete (");
        Serial.print(timeSinceLastSignal);
        Serial.println("ms idle)");
      } else {
        Serial.println("Learning timeout (max 10s reached)");
      }

      // Calculate average burst interval if we got multiple bursts
      uint16_t avgInterval = 0;
      if (capturedRepeats > 0) {
        uint32_t totalTime = lastRepeatTime - firstPressTime;
        avgInterval = totalTime / capturedRepeats;

        Serial.print("Captured ");
        Serial.print(capturedRepeats + 1);  // +1 for total count
        Serial.print(" total bursts, avg interval: ");
        Serial.print(avgInterval);
        Serial.println("ms");
      } else {
        Serial.println("Single burst (no repeats)");
      }

      // Publish the command with burst info
      publishDecode();

      // Publish success
      char msg[128];
      if (capturedRepeats > 0) {
        snprintf(msg, sizeof(msg), "learn_success:%s,bursts:%d", learningCommandName, capturedRepeats + 1);
      } else {
        snprintf(msg, sizeof(msg), "learn_success:%s", learningCommandName);
      }
      mqtt.publish(TOPIC_STATE, msg);
    }

    // Clean up
    IrReceiver.end();  // Stop the receiver
    learnActive = false;
    hasBaseSignal = false;
    capturedRepeats = 0;
    firstPressTime = 0;
    lastRepeatTime = 0;
    lastSignalTime = 0;
    learningCommandName[0] = '\0';  // Clear the name
  }
}

void setup() {
  Serial.begin(115200);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  pinMode(ONBOARD_LED, OUTPUT);  // Initialize LED pin
  // pinMode(INPUT_BUTTON_PIN, INPUT);  // Removed - no longer using button
  while (WiFi.status() != WL_CONNECTED) delay(250);

  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(onMqttMessage);
  mqtt.setBufferSize(2048);  // Increase from default 256 bytes for large raw commands

  // Only initialize sender here, receiver starts on-demand
  IrSender.begin(IR_SEND_PIN);

  Serial.println("ESP32 IR Controller Ready");
}

void loop() {
  if (!mqtt.connected()) ensureMqtt();
  mqtt.loop();

  // Control LED based on learn mode
  if (learnActive) {
    digitalWrite(ONBOARD_LED, HIGH);
  } else {
    digitalWrite(ONBOARD_LED, LOW);
  }

  // Learning mode now triggered via MQTT on TOPIC_LISTEN (see onMqttMessage function)

  handleLearnWindow();
}