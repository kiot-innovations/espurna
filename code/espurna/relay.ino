/*

RELAY MODULE

Copyright (C) 2016-2019 by Xose Pérez <xose dot perez at gmail dot com>

*/

#include <EEPROM_Rotate.h>
#include <Ticker.h>
#include <ArduinoJson.h>
#include <vector>
#include <functional>

typedef struct {

    unsigned char num;
    bool reverse;
    unsigned char status;
    unsigned long _lastTriggred;
    bool _report;
    unsigned char last_reported_status;
    bool take_dimmer_action;
    // Configuration variables

    unsigned char pin;          // GPIO pin for the relay
    unsigned char type;         // RELAY_TYPE_NORMAL, RELAY_TYPE_INVERSE, RELAY_TYPE_LATCHED or RELAY_TYPE_LATCHED_INVERSE
    unsigned char reset_pin;    // GPIO to reset the relay if RELAY_TYPE_LATCHED
    unsigned long delay_on;     // Delay to turn relay ON
    unsigned long delay_off;    // Delay to turn relay OFF
    unsigned char pulse;        // RELAY_PULSE_NONE, RELAY_PULSE_OFF or RELAY_PULSE_ON
    unsigned long pulse_ms;     // Pulse length in millis

    // Status variables

    bool current_status;        // Holds the current (physical) status of the relay
    bool target_status;         // Holds the target status
    unsigned char lock;         // Holds the value of target status, that cannot be changed afterwards. (0 for false, 1 for true, 2 to disable)
    unsigned long fw_start;     // Flood window start time
    unsigned char fw_count;     // Number of changes within the current flood window
    unsigned long change_time;  // Scheduled time to change
    bool report;                // Whether to report to own topic
    bool group_report;          // Whether to report to group topic

    // Helping objects

    Ticker pulseTicker;         // Holds the pulse back timer

} relay_t;
std::vector<relay_t> _relays;
bool _relayRecursive = false;
Ticker _relaySaveTicker;

typedef struct
{
    unsigned char num;
    unsigned char pin;
    unsigned char value;
    bool disabled;
    int vdimval;
} dimmer_t;
dimmer_t _dimmer;

#ifdef V_DIMMER
int dbd = DIMMER_DEBOUNCE_DELAY;
#endif

// -----------------------------------------------------------------------------
// RELAY PROVIDERS
// -----------------------------------------------------------------------------

void _relayProviderStatus(unsigned char id, bool status) {

    // Check relay ID
    if (id >= _relays.size()) return;

    // Store new current status
    _relays[id].current_status = status;

    #if RELAY_PROVIDER == RELAY_PROVIDER_RFBRIDGE
        rfbStatus(id, status);
    #endif

    #if RELAY_PROVIDER == RELAY_PROVIDER_DUAL

        // Calculate mask
        unsigned char mask=0;
        for (unsigned char i=0; i<_relays.size(); i++) {
            if (_relays[i].current_status) mask = mask + (1 << i);
        }

        DEBUG_MSG_P(PSTR("[RELAY] [DUAL] Sending relay mask: %d\n"), mask);

        // Send it to F330
        Serial.flush();
        Serial.write(0xA0);
        Serial.write(0x04);
        Serial.write(mask);
        Serial.write(0xA1);
        Serial.flush();

    #endif

    #if RELAY_PROVIDER == RELAY_PROVIDER_STM
        Serial.flush();
        Serial.write(0xA0);
        Serial.write(id + 1);
        Serial.write(status);
        Serial.write(0xA1 + status + id);

        // The serial init are not full recognized by relais board.
        // References: https://github.com/xoseperez/espurna/issues/1519 , https://github.com/xoseperez/espurna/issues/1130
        delay(100);

        Serial.flush();
    #endif

    #if RELAY_PROVIDER == RELAY_PROVIDER_LIGHT

        // Real relays
        uint8_t physical = _relays.size() - DUMMY_RELAY_COUNT;

        // Support for a mixed of dummy and real relays
        // Reference: https://github.com/xoseperez/espurna/issues/1305
        if (id >= physical) {

            // If the number of dummy relays matches the number of light channels
            // assume each relay controls one channel.
            // If the number of dummy relays is the number of channels plus 1
            // assume the first one controls all the channels and
            // the rest one channel each.
            // Otherwise every dummy relay controls all channels.
            if (DUMMY_RELAY_COUNT == lightChannels()) {
                lightState(id-physical, status);
                lightState(true);
            } else if (DUMMY_RELAY_COUNT == (lightChannels() + 1u)) {
                if (id == physical) {
                    lightState(status);
                } else {
                    lightState(id-1-physical, status);
                }
            } else {
                lightState(status);
            }

            lightUpdate(true, true);
            return;
        
        }

    #endif

    #if (RELAY_PROVIDER == RELAY_PROVIDER_RELAY) || (RELAY_PROVIDER == RELAY_PROVIDER_LIGHT)

        // If this is a light, all dummy relays have already been processed above
        // we reach here if the user has toggled a physical relay

        if (_relays[id].type == RELAY_TYPE_NORMAL) {
            digitalWrite(_relays[id].pin, status);
        } else if (_relays[id].type == RELAY_TYPE_INVERSE) {
            digitalWrite(_relays[id].pin, !status);
        } else if (_relays[id].type == RELAY_TYPE_LATCHED || _relays[id].type == RELAY_TYPE_LATCHED_INVERSE) {
            bool pulse = RELAY_TYPE_LATCHED ? HIGH : LOW;
            digitalWrite(_relays[id].pin, !pulse);
            if (GPIO_NONE != _relays[id].reset_pin) digitalWrite(_relays[id].reset_pin, !pulse);
            if (status || (GPIO_NONE == _relays[id].reset_pin)) {
                digitalWrite(_relays[id].pin, pulse);
            } else {
                digitalWrite(_relays[id].reset_pin, pulse);
            }
            nice_delay(RELAY_LATCHING_PULSE);
            digitalWrite(_relays[id].pin, !pulse);
            if (GPIO_NONE != _relays[id].reset_pin) digitalWrite(_relays[id].reset_pin, !pulse);
        }

    #endif

}

/**
 * Walks the relay vector processing only those relays
 * that have to change to the requested mode
 * @bool mode Requested mode
 */
void _relayProcess(bool mode) {

    unsigned long current_time = millis();

    for (unsigned char id = 0; id < _relays.size(); id++) {

        bool target = _relays[id].target_status;

        // Only process the relays we have to change
        if (target == _relays[id].current_status) continue;

        // Only process the relays we have to change to the requested mode
        if (target != mode) continue;

        // Only process the relays that can be changed
        switch (_relays[id].lock) {
            case RELAY_LOCK_ON:
            case RELAY_LOCK_OFF:
                {
                    bool lock = _relays[id].lock == 1;
                    if (lock != _relays[id].target_status) {
                        _relays[id].target_status = lock;
                        continue;
                    }
                    break;
                }
            case RELAY_LOCK_DISABLED:
            default:
                break;
        }

        // Only process if the change_time has arrived
        if (current_time < _relays[id].change_time) continue;

        DEBUG_MSG_P(PSTR("[RELAY] #%d set to %s\n"), id, target ? "ON" : "OFF");

        // Call the provider to perform the action
        _relayProviderStatus(id, target);

        // Send to Broker
        #if BROKER_SUPPORT
            brokerPublish(BROKER_MSG_TYPE_STATUS, MQTT_TOPIC_RELAY, id, target ? "1" : "0");
        #endif

        // Send MQTT
        #if MQTT_SUPPORT
            relayMQTT(id);
        #endif

        if (!_relayRecursive) {

            relayPulse(id);

            // We will trigger a eeprom save only if
            // we care about current relay status on boot
            unsigned char boot_mode = getSetting("relayBoot", id, RELAY_BOOT_MODE).toInt();
            bool save_eeprom = ((RELAY_BOOT_SAME == boot_mode) || (RELAY_BOOT_TOGGLE == boot_mode));
            _relaySaveTicker.once_ms(RELAY_SAVE_DELAY, relaySave, save_eeprom);

            #if WEB_SUPPORT
                wsPost(_relayWebSocketUpdate);
            #endif

        }

        _relays[id].report = false;
        _relays[id].group_report = false;

    }

}

#if defined(ITEAD_SONOFF_IFAN02)

unsigned char _relay_ifan02_speeds[] = {0, 1, 3, 5};

unsigned char getSpeed() {
    unsigned char speed =
        (_relays[1].target_status ? 1 : 0) +
        (_relays[2].target_status ? 2 : 0) +
        (_relays[3].target_status ? 4 : 0);
    for (unsigned char i=0; i<4; i++) {
        if (_relay_ifan02_speeds[i] == speed) return i;
    }
    return 0;
}

void setSpeed(unsigned char speed) {
    if ((0 <= speed) & (speed <= 3)) {
        if (getSpeed() == speed) return;
        unsigned char states = _relay_ifan02_speeds[speed];
        for (unsigned char i=0; i<3; i++) {
            relayStatus(i+1, states & 1 == 1);
            states >>= 1;
        }
    }
}

#endif

// -----------------------------------------------------------------------------
// RELAY
// -----------------------------------------------------------------------------

void _relayMaskRtcmem(uint32_t mask) {
    Rtcmem->relay = mask;
}

uint32_t _relayMaskRtcmem() {
    return Rtcmem->relay;
}

void relayPulse(unsigned char id) {

    _relays[id].pulseTicker.detach();

    byte mode = _relays[id].pulse;
    if (mode == RELAY_PULSE_NONE) return;
    unsigned long ms = _relays[id].pulse_ms;
    if (ms == 0) return;

    bool status = relayStatus(id);
    bool pulseStatus = (mode == RELAY_PULSE_ON);

    if (pulseStatus != status) {
        DEBUG_MSG_P(PSTR("[RELAY] Scheduling relay #%d back in %lums (pulse)\n"), id, ms);
        _relays[id].pulseTicker.once_ms(ms, relayToggle, id);
        // Reconfigure after dynamic pulse
        _relays[id].pulse = getSetting("relayPulse", id, RELAY_PULSE_MODE).toInt();
        _relays[id].pulse_ms = 1000 * getSetting("relayTime", id, RELAY_PULSE_MODE).toFloat();
    }

}

bool relayStatus(unsigned char id, bool status, bool report, bool group_report) {

    if (id >= _relays.size()) return false;

    bool changed = false;

    if (_relays[id].current_status == status) {

        if (_relays[id].target_status != status) {
            DEBUG_MSG_P(PSTR("[RELAY] #%d scheduled change cancelled\n"), id);
            _relays[id].target_status = status;
            _relays[id].report = false;
            _relays[id].group_report = false;
            changed = true;
        }

        // For RFBridge, keep sending the message even if the status is already the required
        #if RELAY_PROVIDER == RELAY_PROVIDER_RFBRIDGE
            rfbStatus(id, status);
        #endif

        // Update the pulse counter if the relay is already in the non-normal state (#454)
        relayPulse(id);

    } else {

        unsigned long current_time = millis();
        unsigned long fw_end = _relays[id].fw_start + 1000 * RELAY_FLOOD_WINDOW;
        unsigned long delay = status ? _relays[id].delay_on : _relays[id].delay_off;

        _relays[id].fw_count++;
        _relays[id].change_time = current_time + delay;

        // If current_time is off-limits the floodWindow...
        if (current_time < _relays[id].fw_start || fw_end <= current_time) {

            // We reset the floodWindow
            _relays[id].fw_start = current_time;
            _relays[id].fw_count = 1;

        // If current_time is in the floodWindow and there have been too many requests...
        } else if (_relays[id].fw_count >= RELAY_FLOOD_CHANGES) {

            // We schedule the changes to the end of the floodWindow
            // unless it's already delayed beyond that point
            if (fw_end - delay > current_time) {
                _relays[id].change_time = fw_end;
            }

        }

        _relays[id].target_status = status;
        if (report) _relays[id].report = true;
        if (group_report) _relays[id].group_report = true;

        relaySync(id);

        DEBUG_MSG_P(PSTR("[RELAY] #%d scheduled %s in %u ms\n"),
                id, status ? "ON" : "OFF",
                (_relays[id].change_time - current_time));

        changed = true;

    }

    return changed;

}

bool relayStatus(unsigned char id, bool status) {
    return relayStatus(id, status, mqttForward(), true);
}

bool relayStatus(unsigned char id) {

    // Check relay ID
    if (id >= _relays.size()) return false;

    // Get status from storage
    return _relays[id].current_status;

}

void relaySync(unsigned char id) {

    // No sync if none or only one relay
    if (_relays.size() < 2) return;

    // Do not go on if we are comming from a previous sync
    if (_relayRecursive) return;

    // Flag sync mode
    _relayRecursive = true;

    byte relaySync = getSetting("relaySync", RELAY_SYNC).toInt();
    bool status = _relays[id].target_status;

    // If RELAY_SYNC_SAME all relays should have the same state
    if (relaySync == RELAY_SYNC_SAME) {
        for (unsigned short i=0; i<_relays.size(); i++) {
            if (i != id) relayStatus(i, status);
        }

    // If RELAY_SYNC_FIRST all relays should have the same state as first if first changes
    } else if (relaySync == RELAY_SYNC_FIRST) {
        if (id == 0) {
            for (unsigned short i=1; i<_relays.size(); i++) {
                relayStatus(i, status);
            }
        }

    // If NONE_OR_ONE or ONE and setting ON we should set OFF all the others
    } else if (status) {
        if (relaySync != RELAY_SYNC_ANY) {
            for (unsigned short i=0; i<_relays.size(); i++) {
                if (i != id) relayStatus(i, false);
            }
        }

    // If ONLY_ONE and setting OFF we should set ON the other one
    } else {
        if (relaySync == RELAY_SYNC_ONE) {
            unsigned char i = (id + 1) % _relays.size();
            relayStatus(i, true);
        }
    }

    // Unflag sync mode
    _relayRecursive = false;

}

void relaySave(bool eeprom) {

    auto mask = std::bitset<RELAY_SAVE_MASK_MAX>(0);

    unsigned char count = relayCount();
    if (count > RELAY_SAVE_MASK_MAX) count = RELAY_SAVE_MASK_MAX;

    for (unsigned int i=0; i < count; ++i) {
        mask.set(i, relayStatus(i));
    }

    const uint32_t mask_value = mask.to_ulong();

    DEBUG_MSG_P(PSTR("[RELAY] Setting relay mask: %u\n"), mask_value);

    // Persist only to rtcmem, unless requested to save to the eeprom
    _relayMaskRtcmem(mask_value);

    // The 'eeprom' flag controls wether we are commiting this change or not.
    // It is useful to set it to 'false' if the relay change triggering the
    // save involves a relay whose boot mode is independent from current mode,
    // thus storing the last relay value is not absolutely necessary.
    // Nevertheless, we store the value in the EEPROM buffer so it will be written
    // on the next commit.
    if (eeprom) {
        EEPROMr.write(EEPROM_RELAY_STATUS, mask_value);
        // We are actually enqueuing the commit so it will be
        // executed on the main loop, in case this is called from a system context callback
        eepromCommit();
    }

}

void relaySave() {
    relaySave(false);
}

void relayToggle(unsigned char id, bool report, bool group_report) {
    if (id >= _relays.size()) return;
    relayStatus(id, !relayStatus(id), report, group_report);
}

void relayToggle(unsigned char id) {
    relayToggle(id, mqttForward(), true);
}

unsigned char relayCount() {
    return _relays.size();
}
// int jsonParser(const char* payload) {
//     // Parse response
//     StaticJsonBuffer<1500> jsonBuffer;
//     JsonObject &root = jsonBuffer.parseObject(reinterpret_cast<const char *>(payload));

//     if (!root.success()) {
//         DEBUG_MSG_P(PSTR("[API] JSON parsing error\n"));
//         return -1;
//     }

//         return root["value"];
    
// }

unsigned char relayParsePayload(const char * payload) {

    // Payload could be "OFF", "ON", "TOGGLE"
    // or its number equivalents: 0, 1 or 2

    if (payload[0] == '0') return 0;
    if (payload[0] == '1') return 1;
    if (payload[0] == '2') return 2;

    // trim payload
    char * p = ltrim((char *)payload);

    // to lower
    unsigned int l = strlen(p);
    if (l>6) l=6;
    for (unsigned char i=0; i<l; i++) {
        p[i] = tolower(p[i]);
    }

    unsigned int value = 0xFF;
    if (strcmp(p, "off") == 0) {
        value = 0;
    } else if (strcmp(p, "on") == 0) {
        value = 1;
    } else if (strcmp(p, "toggle") == 0) {
        value = 2;
    } else if (strcmp(p, "query") == 0) {
        value = 3;
    }

    return value;

}

// BACKWARDS COMPATIBILITY
void _relayBackwards() {

    for (unsigned int i=0; i<_relays.size(); i++) {
        if (!hasSetting("mqttGroupInv", i)) continue;
        setSetting("mqttGroupSync", i, getSetting("mqttGroupInv", i));
        delSetting("mqttGroupInv", i);
    }

}

void _relayBoot() {

    _relayRecursive = true;
    bool trigger_save = false;
    uint32_t stored_mask = 0;

    if (rtcmemStatus()) {
        stored_mask = _relayMaskRtcmem();
    } else {
        stored_mask = EEPROMr.read(EEPROM_RELAY_STATUS);
    }

    DEBUG_MSG_P(PSTR("[RELAY] Retrieving mask: %u\n"), stored_mask);

    auto mask = std::bitset<RELAY_SAVE_MASK_MAX>(stored_mask);

    // Walk the relays
    unsigned char lock;
    bool status;
    for (unsigned char i=0; i<relayCount(); ++i) {

        unsigned char boot_mode = getSetting("relayBoot", i, RELAY_BOOT_MODE).toInt();
        DEBUG_MSG_P(PSTR("[RELAY] Relay #%u boot mode %u\n"), i, boot_mode);

        status = false;
        lock = RELAY_LOCK_DISABLED;
        switch (boot_mode) {
            case RELAY_BOOT_SAME:
                if (i < 8) {
                    status = mask.test(i);
                }
                break;
            case RELAY_BOOT_TOGGLE:
                if (i < 8) {
                    status = !mask[i];
                    mask.flip(i);
                    trigger_save = true;
                }
                break;
            case RELAY_BOOT_LOCKED_ON:
                status = true;
                lock = RELAY_LOCK_ON;
                break;
            case RELAY_BOOT_LOCKED_OFF:
                lock = RELAY_LOCK_OFF;
                break;
            case RELAY_BOOT_ON:
                status = true;
                break;
            case RELAY_BOOT_OFF:
            default:
                break;
        }

        _relays[i].current_status = !status;
        _relays[i].target_status = status;
        #if RELAY_PROVIDER == RELAY_PROVIDER_STM
            _relays[i].change_time = millis() + 3000 + 1000 * i;
        #else
            _relays[i].change_time = millis();
        #endif

        _relays[i].lock = lock;

     }

    // Save if there is any relay in the RELAY_BOOT_TOGGLE mode
    if (trigger_save) {
        _relayMaskRtcmem(mask.to_ulong());

        EEPROMr.write(EEPROM_RELAY_STATUS, mask.to_ulong());
        eepromCommit();
    }

    _relayRecursive = false;

}

void _relayConfigure() {
    for (unsigned int i=0; i<_relays.size(); i++) {
        _relays[i].pulse = getSetting("relayPulse", i, RELAY_PULSE_MODE).toInt();
        _relays[i].pulse_ms = 1000 * getSetting("relayTime", i, RELAY_PULSE_MODE).toFloat();

        if (GPIO_NONE == _relays[i].pin) continue;

        pinMode(_relays[i].pin, OUTPUT);
        if (GPIO_NONE != _relays[i].reset_pin) {
            pinMode(_relays[i].reset_pin, OUTPUT);
        }
        if (_relays[i].type == RELAY_TYPE_INVERSE) {
            //set to high to block short opening of relay
            digitalWrite(_relays[i].pin, HIGH);
        }
    }
}

//------------------------------------------------------------------------------
// WEBSOCKETS
//------------------------------------------------------------------------------

#if WEB_SUPPORT

bool _relayWebSocketOnKeyCheck(const char * key, JsonVariant& value) {
    return (strncmp(key, "relay", 5) == 0);
}

void _relayWebSocketUpdate(JsonObject& root) {
    JsonObject& state = root.createNestedObject("relayState");
    state["size"] = relayCount();

    JsonArray& status = state.createNestedArray("status");
    JsonArray& lock = state.createNestedArray("lock");

    for (unsigned char i=0; i<relayCount(); i++) {
        status.add<uint8_t>(_relays[i].target_status);
        lock.add(_relays[i].lock);
    }
}

String _relayFriendlyName(unsigned char i) {
    String res = String("GPIO") + String(_relays[i].pin);

    if (GPIO_NONE == _relays[i].pin) {
        #if (RELAY_PROVIDER == RELAY_PROVIDER_LIGHT)
            uint8_t physical = _relays.size() - DUMMY_RELAY_COUNT;
            if (i >= physical) {
                if (DUMMY_RELAY_COUNT == lightChannels()) {
                    res = String("CH") + String(i-physical);
                } else if (DUMMY_RELAY_COUNT == (lightChannels() + 1u)) {
                    if (physical == i) {
                        res = String("Light");
                    } else {
                        res = String("CH") + String(i-1-physical);
                    }
                } else {
                    res = String("Light");
                }
            } else {
                res = String("?");
            }
        #else
            res = String("SW") + String(i);
        #endif
    }

    return res;
}

void _relayWebSocketSendRelays(JsonObject& root) {
    JsonObject& relays = root.createNestedObject("relayConfig");

    relays["size"] = relayCount();
    relays["start"] = 0;

    JsonArray& gpio = relays.createNestedArray("gpio");
    JsonArray& type = relays.createNestedArray("type");
    JsonArray& reset = relays.createNestedArray("reset");
    JsonArray& boot = relays.createNestedArray("boot");
    JsonArray& pulse = relays.createNestedArray("pulse");
    JsonArray& pulse_time = relays.createNestedArray("pulse_time");

    #if MQTT_SUPPORT
        JsonArray& group = relays.createNestedArray("group");
        JsonArray& group_sync = relays.createNestedArray("group_sync");
        JsonArray& on_disconnect = relays.createNestedArray("on_disc");
    #endif

    for (unsigned char i=0; i<relayCount(); i++) {
        gpio.add(_relayFriendlyName(i));

        type.add(_relays[i].type);
        reset.add(_relays[i].reset_pin);
        boot.add(getSetting("relayBoot", i, RELAY_BOOT_MODE).toInt());

        pulse.add(_relays[i].pulse);
        pulse_time.add(_relays[i].pulse_ms / 1000.0);

        #if MQTT_SUPPORT
            group.add(getSetting("mqttGroup", i, ""));
            group_sync.add(getSetting("mqttGroupSync", i, 0).toInt());
            on_disconnect.add(getSetting("relayOnDisc", i, 0).toInt());
        #endif
    }
}

void _relayWebSocketOnVisible(JsonObject& root) {
    if (relayCount() == 0) return;

    if (relayCount() > 1) {
        root["multirelayVisible"] = 1;
        root["relaySync"] = getSetting("relaySync", RELAY_SYNC);
    }

    root["relayVisible"] = 1;
}

void _relayWebSocketOnConnected(JsonObject& root) {

    if (relayCount() == 0) return;

    // Per-relay configuration
    _relayWebSocketSendRelays(root);

}

void _relayWebSocketOnAction(uint32_t client_id, const char * action, JsonObject& data) {

    if (strcmp(action, "relay") != 0) return;

    if (data.containsKey("status")) {

        unsigned char value = relayParsePayload(data["status"]);

        if (value == 3) {

            wsPost(_relayWebSocketUpdate);

        } else if (value < 3) {

            unsigned int relayID = 0;
            if (data.containsKey("id")) {
                String value = data["id"];
                relayID = value.toInt();
            }

            // Action to perform
            if (value == 0) {
                relayStatus(relayID, false);
            } else if (value == 1) {
                relayStatus(relayID, true);
            } else if (value == 2) {
                relayToggle(relayID);
            }

        }

    }

}

void relaySetupWS() {
    wsRegister()
        .onVisible(_relayWebSocketOnVisible)
        .onConnected(_relayWebSocketOnConnected)
        .onData(_relayWebSocketUpdate)
        .onAction(_relayWebSocketOnAction)
        .onKeyCheck(_relayWebSocketOnKeyCheck);
}

#endif // WEB_SUPPORT

//------------------------------------------------------------------------------
// REST API
//------------------------------------------------------------------------------

#if API_SUPPORT

void relaySetupAPI() {

    char key[20];

    // API entry for dimmer
    #ifdef V_DIMMER
        snprintf_P(key, sizeof(key), PSTR("%s"), MQTT_TOPIC_DIMMER);
        apiRegister(
            key,
            [](char * buffer, size_t len) {
				snprintf_P(buffer, len, PSTR("%d"), dimmerProviderStatus());
            },
            [](const char * payload) {
                // uint8_t value = jsonParser(payload);
                dimmerProviderStatus(String(payload).toInt());
            }
        );
    #endif
    // API entry points (protected with apikey)
    for (unsigned int relayID=0; relayID<relayCount(); relayID++) {

        snprintf_P(key, sizeof(key), PSTR("%s/%d"), MQTT_TOPIC_RELAY, relayID);
        apiRegister(key,
            [relayID](char * buffer, size_t len) {
				snprintf_P(buffer, len, PSTR("%d"), _relays[relayID].target_status ? 1 : 0);
            },
            [relayID](const char * payload) {

                uint8_t value = relayParsePayload(payload);

                if (value == 0xFF) {
                    DEBUG_MSG_P(PSTR("[RELAY] Wrong payload (%s)\n"), payload);
                    return;
                }

                if (value == 0) {
                    relayStatus(relayID, false);
                } else if (value == 1) {
                    relayStatus(relayID, true);
                } else if (value == 2) {
                    relayToggle(relayID);
                }

            }
        );

        snprintf_P(key, sizeof(key), PSTR("%s/%d"), MQTT_TOPIC_PULSE, relayID);
        apiRegister(key,
            [relayID](char * buffer, size_t len) {
                dtostrf((double) _relays[relayID].pulse_ms / 1000, 1, 3, buffer);
            },
            [relayID](const char * payload) {

                unsigned long pulse = 1000 * atof(payload);
                if (0 == pulse) return;

                if (RELAY_PULSE_NONE != _relays[relayID].pulse) {
                    DEBUG_MSG_P(PSTR("[RELAY] Overriding relay #%d pulse settings\n"), relayID);
                }

                _relays[relayID].pulse_ms = pulse;
                _relays[relayID].pulse = relayStatus(relayID) ? RELAY_PULSE_ON : RELAY_PULSE_OFF;
                relayToggle(relayID, true, false);

            }
        );

        #if defined(ITEAD_SONOFF_IFAN02)

            apiRegister(MQTT_TOPIC_SPEED,
                [relayID](char * buffer, size_t len) {
                    snprintf(buffer, len, "%u", getSpeed());
                },
                [relayID](const char * payload) {
                    setSpeed(atoi(payload));
                }
            );

        #endif

    }

}

#endif // API_SUPPORT

//------------------------------------------------------------------------------
// MQTT
//------------------------------------------------------------------------------

#if MQTT_SUPPORT

void _relayMQTTGroup(unsigned char id) {
    String topic = getSetting("mqttGroup", id, "");
    if (!topic.length()) return;

    unsigned char mode = getSetting("mqttGroupSync", id, RELAY_GROUP_SYNC_NORMAL).toInt();
    if (mode == RELAY_GROUP_SYNC_RECEIVEONLY) return;

    bool status = relayStatus(id);
    if (mode == RELAY_GROUP_SYNC_INVERSE) status = !status;
    mqttSendRaw(topic.c_str(), status ? RELAY_MQTT_ON : RELAY_MQTT_OFF);
}

void relayMQTT(unsigned char id) {

    if (id >= _relays.size()) return;

    // Send state topic
    if (_relays[id].report) {
        _relays[id].report = false;
        mqttSend(MQTT_TOPIC_RELAY, id, _relays[id].current_status ? RELAY_MQTT_ON : RELAY_MQTT_OFF);
    }

    // Check group topic
    if (_relays[id].group_report) {
        _relays[id].group_report = false;
        _relayMQTTGroup(id);
    }

    // Send speed for IFAN02
    #if defined (ITEAD_SONOFF_IFAN02)
        char buffer[5];
        snprintf(buffer, sizeof(buffer), "%u", getSpeed());
        mqttSend(MQTT_TOPIC_SPEED, buffer);
    #endif

}

void relayMQTT() {
    for (unsigned int id=0; id < _relays.size(); id++) {
        mqttSend(MQTT_TOPIC_RELAY, id, _relays[id].current_status ? RELAY_MQTT_ON : RELAY_MQTT_OFF);
    }
}

void relayStatusWrap(unsigned char id, unsigned char value, bool is_group_topic) {
    switch (value) {
        case 0:
            relayStatus(id, false, mqttForward(), !is_group_topic);
            break;
        case 1:
            relayStatus(id, true, mqttForward(), !is_group_topic);
            break;
        case 2:
            relayToggle(id, true, true);
            break;
        default:
            _relays[id].report = true;
            relayMQTT(id);
            break;
    }
}

void relayMQTTCallback(unsigned int type, const char * topic, const char * payload) {

    if (type == MQTT_CONNECT_EVENT) {

        // Send status on connect
        #if (HEARTBEAT_MODE == HEARTBEAT_NONE) or (not HEARTBEAT_REPORT_RELAY)
            relayMQTT();
        #endif

        // Subscribe to own /set topic
        #ifdef IS_KIOT_PAYLOAD
        char relay_topic[strlen(MQTT_TOPIC_RELAY) + 2];
        snprintf_P(relay_topic, sizeof(relay_topic), PSTR("%s/"), MQTT_TOPIC_RELAY);
        mqttSubscribe(relay_topic);
        #else
        char relay_topic[strlen(MQTT_TOPIC_RELAY) + 3];
        snprintf_P(relay_topic, sizeof(relay_topic), PSTR("%s/+"), MQTT_TOPIC_RELAY);
        mqttSubscribe(relay_topic);
        #endif

        // Subscribe to pulse topic
        char pulse_topic[strlen(MQTT_TOPIC_PULSE) + 3];
        snprintf_P(pulse_topic, sizeof(pulse_topic), PSTR("%s/+"), MQTT_TOPIC_PULSE);
        mqttSubscribe(pulse_topic);

        #if defined(ITEAD_SONOFF_IFAN02)
            mqttSubscribe(MQTT_TOPIC_SPEED);
        #endif

        // Subscribe to group topics
        for (unsigned int i=0; i < _relays.size(); i++) {
            String t = getSetting("mqttGroup", i, "");
            if (t.length() > 0) mqttSubscribeRaw(t.c_str());
        }

    }

    if (type == MQTT_MESSAGE_EVENT) {

        String t = mqttMagnitude((char *) topic);

        // magnitude is relay/#/pulse
        if (t.startsWith(MQTT_TOPIC_PULSE)) {

            unsigned int id = t.substring(strlen(MQTT_TOPIC_PULSE)+1).toInt();

            if (id >= relayCount()) {
                DEBUG_MSG_P(PSTR("[RELAY] Wrong relayID (%d)\n"), id);
                return;
            }

            unsigned long pulse = 1000 * atof(payload);
            if (0 == pulse) return;

            if (RELAY_PULSE_NONE != _relays[id].pulse) {
                DEBUG_MSG_P(PSTR("[RELAY] Overriding relay #%d pulse settings\n"), id);
            }

            _relays[id].pulse_ms = pulse;
            _relays[id].pulse = relayStatus(id) ? RELAY_PULSE_ON : RELAY_PULSE_OFF;
            relayToggle(id, true, false);

            return;

        }

        // magnitude is relay/#
        if (t.startsWith(MQTT_TOPIC_RELAY)) {

            #ifdef IS_KIOT_PAYLOAD

                StaticJsonBuffer<500> jsonBuffer;
                JsonObject &root = jsonBuffer.parseObject(reinterpret_cast<const char *>(payload));
                if (!root.success()) {
                    DEBUG_MSG_P(PSTR("[ERROR] parseObject() failed for MQTT NOw"));
                    return;
                }

                int action = root["action"];
                // Get relay ID
                unsigned int id = action / 10;
                if (id >= relayCount()) {
                    DEBUG_MSG_P(PSTR("[RELAY] Wrong relayID (%d)\n"), id);
                    return;
                }

                // Get value
                unsigned char value = (unsigned char)(action % 10);
                relayStatusWrap(id, value, false);

                return;
            #else
                // Get relay ID
                unsigned int id = t.substring(strlen(MQTT_TOPIC_RELAY)+1).toInt();
                if (id >= relayCount()) {
                    DEBUG_MSG_P(PSTR("[RELAY] Wrong relayID (%d)\n"), id);
                    return;
                }

                // Get value
                unsigned char value = relayParsePayload(payload);
                if (value == 0xFF) return;

                relayStatusWrap(id, value, false);

                return;
            #endif
        }


        // Check group topics
        for (unsigned int i=0; i < _relays.size(); i++) {

            String t = getSetting("mqttGroup", i, "");

            if ((t.length() > 0) && t.equals(topic)) {

                unsigned char value = relayParsePayload(payload);
                if (value == 0xFF) return;

                if (value < 2) {
                    if (getSetting("mqttGroupSync", i, RELAY_GROUP_SYNC_NORMAL).toInt() == RELAY_GROUP_SYNC_INVERSE) {
                        value = 1 - value;
                    }
                }

                DEBUG_MSG_P(PSTR("[RELAY] Matched group topic for relayID %d\n"), i);
                relayStatusWrap(i, value, true);

            }
        }

        // Itead Sonoff IFAN02
        #if defined (ITEAD_SONOFF_IFAN02)
            if (t.startsWith(MQTT_TOPIC_SPEED)) {
                setSpeed(atoi(payload));
            }
        #endif

    }

    if (type == MQTT_DISCONNECT_EVENT) {
        for (unsigned int i=0; i < _relays.size(); i++){
            int reaction = getSetting("relayOnDisc", i, 0).toInt();
            if (1 == reaction) {     // switch relay OFF
                DEBUG_MSG_P(PSTR("[RELAY] Reset relay (%d) due to MQTT disconnection\n"), i);
                relayStatusWrap(i, false, false);
            } else if(2 == reaction) { // switch relay ON
                DEBUG_MSG_P(PSTR("[RELAY] Set relay (%d) due to MQTT disconnection\n"), i);
                relayStatusWrap(i, true, false);
            }
        }

    }

}

void relaySetupMQTT() {
    mqttRegister(relayMQTTCallback);
}

#endif

//------------------------------------------------------------------------------
// Settings
//------------------------------------------------------------------------------

#if TERMINAL_SUPPORT

void _relayInitCommands() {

    terminalRegisterCommand(F("RELAY"), [](Embedis* e) {
        if (e->argc < 2) {
            terminalError(F("Wrong arguments"));
            return;
        }
        int id = String(e->argv[1]).toInt();
        if (id >= relayCount()) {
            DEBUG_MSG_P(PSTR("-ERROR: Wrong relayID (%d)\n"), id);
            return;
        }

        if (e->argc > 2) {
            int value = String(e->argv[2]).toInt();
            if (value == 2) {
                relayToggle(id);
            } else {
                relayStatus(id, value == 1);
            }
        }
        DEBUG_MSG_P(PSTR("Status: %s\n"), _relays[id].target_status ? "true" : "false");
        if (_relays[id].pulse != RELAY_PULSE_NONE) {
            DEBUG_MSG_P(PSTR("Pulse: %s\n"), (_relays[id].pulse == RELAY_PULSE_ON) ? "ON" : "OFF");
            DEBUG_MSG_P(PSTR("Pulse time: %d\n"), _relays[id].pulse_ms);

        }
        terminalOK();
    });

}

#endif // TERMINAL_SUPPORT

//------------------------------------------------------------------------------
// Setup
//------------------------------------------------------------------------------

void _relayLoop() {
    _relayProcess(false);
    _relayProcess(true);
}

void dimmerEnable() {
    int use_dimmer = getSetting("use_dimmer_1", "1").toInt();
    if (use_dimmer == 0) {
        _dimmer.disabled = true;
        setDimmerToFull();
    }
}
// Special Function - has to be called everytime, for devices which dont have proper atmega code, we need to continously send this.
void setDimmerToFull() {
    nice_delay(10000);
    DEBUG_MSG_P(PSTR("DIM SET %d %d \n"), _dimmer.num, 99);
}

#ifdef V_DIMMER
    int getStabilisedDimmerVal(int time){
        unsigned long start = millis();
        int dim_status = analogRead(V_DIMMER);
        int i = 1;

        while(millis() - start < time) {
            int j = 0;
                    int temp = 0;
                for(j = 0; j < 25; j++){
                    temp+=analogRead(V_DIMMER);
                }
                temp/=25;
                dim_status = dim_status + (temp - dim_status) / i;
                i++;
        }
        dim_status = max(0, dim_status);
        dim_status = getDimValueInRange100(dim_status);
        return dim_status;
    }
#endif
int getDimValueInRange100(int val) {
    #if DIMMING_LEVELS == 4
        if (val > 800) {
            return 99;
        } else if (val > 600) {
            return 75;
        } else if (val > 400) {
            return 50;
        } else if (val > 200) {
            return 25;
        } else {
            return 0;
        }
    #elif DIMMING_LEVELS == 5
        if (val > 900) {
            return 99;
        } else if (val > 800) {
            return 80;
        } else if (val > 600) {
            return 60;
        } else if (val > 400) {
            return 40;
        } else if (val > 200) {
            return 20;
        } else {
            return 0;
        }
    #endif
}

void relayRetrieve(bool invert) {

    #if RELAY_PROVIDER == RELAY_PROVIDER_ESP
        uint8_t status = EEPROMr.read(EEPROM_RELAY_STATUS);
        DEBUG_MSG_P(PSTR("[DEBUG] Relay Status Retreieved : %d \n"), status);
        // relayAllUpdate(status);
    #endif

        if (_dimmer.disabled == true) {
            DEBUG_MSG_P(PSTR("CONFIG DIM DIS %d 1 \n"), _dimmer.num);
            setDimmerToFull();
        } else {
            DEBUG_MSG_P(PSTR("CONFIG DIM DIS %d 0 \n"), _dimmer.num);
        }
}

// // Function for updating Relay status in ram when sent from eeprom;
// void relayAllUpdate(uint8_t status) {
//     unsigned char bit = 1;
//     bool changed = false;
//     for (unsigned int i = 0; i < _relays.size(); i++) {
//         // Do not report this- It's initializing
//         if (relayUpdate(i + 1, ((status & bit) == bit)))
//             changed = true;
//         bit += bit;
//     }

//     if (changed) {
//         relayAllMQTT();
//         #if MQTT_REPORT_EVENT_TO_HOME
//                 activeHomePong(true);
//         #endif
//     }
// }

bool relayUpdate(unsigned char id, unsigned char status) {
    bool changed = false;
    if (id > _relays.size() || id <= 0)
        return false;
    #if RELAY_PROVIDER == RELAY_PROVIDER_ESP
    changed = relayProviderStatus(id, status);
    #endif
    _relays[id - 1].last_reported_status = status;
    return changed;
}

bool relayProviderStatus(unsigned char id, bool status) {
    bool changed = false;
    if (id > _relays.size() || id <= 0)
        return changed;

    #if RELAY_PROVIDER == RELAY_PROVIDER_ESP
        DEBUG_MSG_P(PSTR("R_%d_%d\n"), _relays[id - 1].num, status);
        if (_relays[id - 1].status != status) {
            changed = true;
        }
        if (_relays[id - 1].take_dimmer_action) {
            if (status) {
                // If turning on then simply restore the previous value
                // Force on Dimmer because till now relay.status is set to false.
                dimmerProviderStatus(_dimmer.value, true);
            }
            #if DIMMER_PROVIDER == DIMMER_PROVIDER_ESP
                    else {
                        // If turning off, then turn off all dimmer pins, but do not save it.
                        dimmerOffWithoutSavingDimValue(id);
                    }
            #endif
        } else {
            digitalWrite(_relays[id - 1].pin, status);
        }

        _relays[id - 1].status = status;
    #endif
        return changed;
}

void dimmerProviderStatus(int value, bool force) {
    DEBUG_MSG_P(PSTR("[Dimmer] taking Dimmer action %d "), value);
    if (_dimmer.disabled == true) {
        value = 99;
    }
    if (value < 0) {
        value = 0;
    }
    if (value > 99) {
        value = 99;
    }
    #if DIMMER_PROVIDER == DIMMER_PROVIDER_ESP
        int _dim_value = map(value, 0, 99, 0, DIMMING_LEVELS);
        _dimmer.value = value;
        DEBUG_MSG_P(PSTR("[Dimmer] Setting Dimmer level to : %d \n"), _dim_value);
        if (DIMMING_LEVELS == 5) {
    #if CHEAT_DIMMER
        DEBUG_MSG_P(PSTR("[Dimmer] Coming in cheat dimmer"));
    #endif
            switch (_dim_value) {
    #if CHEAT_DIMMER
                case 0:
                    digitalWrite(DIMMER_PINS[0], LOW);
                    digitalWrite(DIMMER_PINS[1], LOW);
                    digitalWrite(DIMMER_PINS[2], LOW);
                    break;
                case 1:
                    digitalWrite(DIMMER_PINS[0], HIGH);
                    digitalWrite(DIMMER_PINS[1], LOW);
                    digitalWrite(DIMMER_PINS[2], LOW);
                    break;
                // SPEED IN BOTH LEVELS Level 2 and Level 3 would remain the same.
                case 2:
                    digitalWrite(DIMMER_PINS[0], LOW);
                    digitalWrite(DIMMER_PINS[1], HIGH);
                    digitalWrite(DIMMER_PINS[2], LOW);
                    break;
                case 3:
                    digitalWrite(DIMMER_PINS[0], LOW);
                    digitalWrite(DIMMER_PINS[1], HIGH);
                    digitalWrite(DIMMER_PINS[2], LOW);
                    break;
                case 4:
                    digitalWrite(DIMMER_PINS[0], HIGH);
                    digitalWrite(DIMMER_PINS[1], HIGH);
                    digitalWrite(DIMMER_PINS[2], LOW);
                    break;
                case 5:
                    digitalWrite(DIMMER_PINS[0], LOW);
                    digitalWrite(DIMMER_PINS[1], LOW);
                    digitalWrite(DIMMER_PINS[2], HIGH);
                    break;
                default:
                    break;

    #else
                case 0:
                    digitalWrite(DIMMER_PINS[0], LOW);
                    digitalWrite(DIMMER_PINS[1], LOW);
                    digitalWrite(DIMMER_PINS[2], LOW);
                    digitalWrite(DIMMER_PINS[3], LOW);
                    break;
                case 1:
                    digitalWrite(DIMMER_PINS[0], HIGH);
                    digitalWrite(DIMMER_PINS[1], LOW);
                    digitalWrite(DIMMER_PINS[2], LOW);
                    digitalWrite(DIMMER_PINS[3], LOW);
                    break;
                case 2:
                    digitalWrite(DIMMER_PINS[0], LOW);
                    digitalWrite(DIMMER_PINS[1], HIGH);
                    digitalWrite(DIMMER_PINS[2], LOW);
                    digitalWrite(DIMMER_PINS[3], LOW);
                    break;
                case 3:
                    digitalWrite(DIMMER_PINS[0], LOW);
                    digitalWrite(DIMMER_PINS[1], HIGH);
                    digitalWrite(DIMMER_PINS[2], HIGH);
                    digitalWrite(DIMMER_PINS[3], LOW);
                    break;
                case 4:
                    digitalWrite(DIMMER_PINS[0], HIGH);
                    digitalWrite(DIMMER_PINS[1], HIGH);
                    digitalWrite(DIMMER_PINS[2], HIGH);
                    digitalWrite(DIMMER_PINS[3], LOW);
                    break;
                case 5:
                    digitalWrite(DIMMER_PINS[0], LOW);
                    digitalWrite(DIMMER_PINS[1], LOW);
                    digitalWrite(DIMMER_PINS[2], LOW);
                    digitalWrite(DIMMER_PINS[3], HIGH);
                    break;
                default:
                    break;
    #endif
            }
        } else if (DIMMING_LEVELS == 4) {
            switch (_dim_value) {
                case 0:
                    digitalWrite(DIMMER_PINS[0], LOW);
                    digitalWrite(DIMMER_PINS[1], LOW);
                    digitalWrite(DIMMER_PINS[2], LOW);
                    break;
                case 1:
                    digitalWrite(DIMMER_PINS[0], HIGH);
                    digitalWrite(DIMMER_PINS[1], LOW);
                    digitalWrite(DIMMER_PINS[2], LOW);
                    break;
                case 2:
                    digitalWrite(DIMMER_PINS[0], LOW);
                    digitalWrite(DIMMER_PINS[1], HIGH);
                    digitalWrite(DIMMER_PINS[2], LOW);
                    break;
                case 3:
                    digitalWrite(DIMMER_PINS[0], HIGH);
                    digitalWrite(DIMMER_PINS[1], HIGH);
                    digitalWrite(DIMMER_PINS[2], LOW);
                    break;
                case 4:
                    digitalWrite(DIMMER_PINS[0], LOW);
                    digitalWrite(DIMMER_PINS[1], LOW);
                    digitalWrite(DIMMER_PINS[2], HIGH);
                    break;
                default:
                    break;
            }
        }
    #endif
}

#if DIMMER_PROVIDER == DIMMER_PROVIDER_ESP

void dimmerOffWithoutSavingDimValue(unsigned char id) {
    for (int i = 0; i < DIMMER_PINS_COUNT; i++) {
        digitalWrite(DIMMER_PINS[i], LOW);
    }
}

#endif

bool relayProviderStatus(unsigned char id) {

    #if RELAY_PROVIDER == RELAY_PROVIDER_ESP

        if (id > _relays.size() || id <= 0)
            return false;
        bool status = false;
        if (_relays[id - 1].take_dimmer_action) {
            status = _relays[id - 1].status;
        } else {
            status = (digitalRead(_relays[id - 1].pin) == HIGH);
        }
        return status;
    #endif
}

void relayAllMQTT() {
    mqttSend(MQTT_TOPIC_RELAY_1, relayString().c_str());
}

void dimmerRetrieve() {
    #if RELAY_PROVIDER == RELAY_PROVIDER_ESP
        uint8_t dimmer_status = EEPROMr.read(EEPROM_DIMMER_STATUS);
        dimmerProviderStatus((int)dimmer_status);
    #endif
}

void relayString(JsonObject &root) {
    JsonArray &relay = root.createNestedArray("relays");
    for (unsigned char i = 1; i <= relayCount(); i++) {
        relay.add((int)relayStatus(i));
    }
    root["dimmer"] = dimmerProviderStatus();
    return;
}

String relayString() {
    DynamicJsonBuffer jsonBuffer;
    JsonObject &root = jsonBuffer.createObject();
    JsonArray &relay = root.createNestedArray("relays");
    for (unsigned char i = 1; i <= relayCount(); i++) {
        relay.add((int)relayStatus(i));
    }
    #ifdef DIMMER_NUM
        root["dimmer"] = String(dimmerProviderStatus());
    #endif

    String output;
    root.printTo(output);
    return output;
}

int dimmerProviderStatus() {
    if (_dimmer.disabled == true) {
        return 99;
    }
    int value = _dimmer.value;
    return value;
}

void dimmerProviderStatus(int value) {
    dimmerProviderStatus(value, false);
}

void relaySetup() {

    // Ad-hoc relays
    #if RELAY1_PIN != GPIO_NONE
        _relays.push_back((relay_t) { RELAY1_PIN, RELAY1_TYPE, RELAY1_RESET_PIN, RELAY1_DELAY_ON, RELAY1_DELAY_OFF });
    #endif
    #if RELAY2_PIN != GPIO_NONE
        _relays.push_back((relay_t) { RELAY2_PIN, RELAY2_TYPE, RELAY2_RESET_PIN, RELAY2_DELAY_ON, RELAY2_DELAY_OFF });
    #endif
    #if RELAY3_PIN != GPIO_NONE
        _relays.push_back((relay_t) { RELAY3_PIN, RELAY3_TYPE, RELAY3_RESET_PIN, RELAY3_DELAY_ON, RELAY3_DELAY_OFF });
    #endif
    #if RELAY4_PIN != GPIO_NONE
        _relays.push_back((relay_t) { RELAY4_PIN, RELAY4_TYPE, RELAY4_RESET_PIN, RELAY4_DELAY_ON, RELAY4_DELAY_OFF });
    #endif
    #if RELAY5_PIN != GPIO_NONE
        _relays.push_back((relay_t) { RELAY5_PIN, RELAY5_TYPE, RELAY5_RESET_PIN, RELAY5_DELAY_ON, RELAY5_DELAY_OFF });
    #endif
    #if RELAY6_PIN != GPIO_NONE
        _relays.push_back((relay_t) { RELAY6_PIN, RELAY6_TYPE, RELAY6_RESET_PIN, RELAY6_DELAY_ON, RELAY6_DELAY_OFF });
    #endif
    #if RELAY7_PIN != GPIO_NONE
        _relays.push_back((relay_t) { RELAY7_PIN, RELAY7_TYPE, RELAY7_RESET_PIN, RELAY7_DELAY_ON, RELAY7_DELAY_OFF });
    #endif
    #if RELAY8_PIN != GPIO_NONE
        _relays.push_back((relay_t) { RELAY8_PIN, RELAY8_TYPE, RELAY8_RESET_PIN, RELAY8_DELAY_ON, RELAY8_DELAY_OFF });
    #endif
     #ifdef DIMMER_NUM
        _dimmer = {DIMMER_NUM, 0, 0, false, 0};
        dimmerEnable();
    #endif
    #ifdef V_DIMMER
        pinMode(V_DIMMER, INPUT);
    #endif

    #if defined(Nexa_1_PLUS_1)
        _relays[1].take_dimmer_action = true;
        for (int i = 0; i < DIMMER_PINS_COUNT; i++) {
            pinMode(DIMMER_PINS[i], OUTPUT);
        }
    #endif
    #ifdef V_DIMMER
        // int dim_status = analogRead(V_DIMMER);
        // for (int i = 1; i < 35; i++) {
        //     dim_status = dim_status + (analogRead(V_DIMMER) - dim_status) / i;
        // }
        // dim_status = max(0, dim_status);
        // _dimmer.vdimval = map(dim_status, 0, 1024, 0, 99);
        // getSetting("ddd", DIMMER_DEBOUNCE_DELAY);
        _dimmer.vdimval = getStabilisedDimmerVal(20);
        // DEBUG_MSG_P(PSTR("Initial vdimval is this %d \n"), _dimmer.vdimval);
    #endif
    DEBUG_MSG_P(PSTR("[RELAY] Number of relays: %d\n"), _relays.size());

    byte relayMode = getSetting("relayMode", RELAY_BOOT_MODE).toInt();
    for (unsigned int i = 0; i < _relays.size(); i++) {
        #if RELAY_PROVIDER == RELAY_PROVIDER_ESP
                pinMode(_relays[i].pin, OUTPUT);
        #endif
    }
    if (relayMode == RELAY_MODE_SAME) {
        relayRetrieve(false);
        dimmerRetrieve();
    }

    // Dummy relays for AI Light, Magic Home LED Controller, H801, Sonoff Dual and Sonoff RF Bridge
    // No delay_on or off for these devices to easily allow having more than
    // 8 channels. This behaviour will be recovered with v2.
    for (unsigned char i=0; i < DUMMY_RELAY_COUNT; i++) {
        _relays.push_back((relay_t) {GPIO_NONE, RELAY_TYPE_NORMAL, 0, 0, 0});
    }

    _relayBackwards();
    _relayConfigure();
    _relayBoot();
    _relayLoop();

    #if WEB_SUPPORT
        relaySetupWS();
    #endif
    #if API_SUPPORT
        relaySetupAPI();
    #endif
    #if MQTT_SUPPORT
        relaySetupMQTT();
    #endif
    #if TERMINAL_SUPPORT
        _relayInitCommands();
    #endif

    // Main callbacks
    espurnaRegisterLoop(_relayLoop);
    espurnaRegisterReload(_relayConfigure);

    DEBUG_MSG_P(PSTR("[RELAY] Number of relays: %d\n"), _relays.size());

}
