# MqttManager

Single owner of MQTT communication for the framework, talking to the
**home-automation-platform** via an EMQX Cloud broker. MQTT is the only cloud
transport in this framework.

## Dependencies

Install via Arduino Library Manager:

- **PubSubClient** by Nick O'Leary (v2.8+)
- **ArduinoJson** by bblanchon (v7+)

`WiFiClientSecure` is part of the ESP32 core.

## Topic contract

For `deviceId = <id>` (= the board's `hardware_id` in the admin portal):

| Topic | Direction | Payload | Retained |
|---|---|---|---|
| `ha/<id>/relay/<n>/set` | device subscribes | `1` / `0` | no |
| `ha/<id>/relay/<n>` | device publishes | `1` / `0` | yes |
| `ha/<id>/status` | device publishes | JSON | yes |
| `ha/<id>/status` (LWT) | broker publishes on drop | `{"online":false}` | yes |

Full spec: `home-automation-platform/ai-documents/MQTT_CONTRACT.md`.

## API

```cpp
MqttManager mqtt;

mqtt.configure(host, 8883, user, pass, deviceId, caCertOrNull);
mqtt.onEvent(onMqttEvent);       // lifecycle: Connecting/Connected/Disconnected/Error
mqtt.onCommand(onMqttCommand);   // (channel 1-based, bool on) from the platform
mqtt.begin();                    // after WiFi is up

// every loop():
mqtt.loop();

// publishing:
mqtt.publishRelayState(channel, on);                  // retained; queued if offline
mqtt.publishStatus(fw, rssiDbm, heap, bootCount, upS); // retained; dropped if offline
```

On every (re)connect the manager fires `MqttEvent::Connected` — republish all
relay states + status there (see `sketch_july21`).

## Design notes

- **No heap.** Static buffers only; `WiFiClientSecure` + `PubSubClient` are value
  members. Offline relay-state publishes use a fixed FIFO ring (`MQTT_MAX_QUEUE`).
- **TLS.** Pass a PEM CA to `configure()` to validate the broker; pass `nullptr`
  to skip validation (`setInsecure()`) for first bring-up only.
- **Contextless callback bridge.** PubSubClient's message callback has no user
  context, so a single `s_instance` static forwards to the instance — the same
  single-instance static bridge pattern.
- **No self-echo.** The device subscribes only to `…/relay/<n>/set`, never to its
  own state topic, so physical toggles and cloud commands never fight.
