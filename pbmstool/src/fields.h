#pragma once
#include "bms.h"
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace pace {

using KeyValue  = std::pair<std::string, std::string>;
using KeyValues = std::vector<KeyValue>;

// Ordered key/value pairs for the `analog` subcommand. The CLI prints each
// pair as `key=value`; the daemon publishes a subset of them to MQTT.
KeyValues analog_fields(uint8_t pack, const AnalogData& a);

// Ordered key/value pairs for the `alarm` subcommand (same contract).
KeyValues alarm_fields(uint8_t pack, const AlarmData& al);

} // namespace pace
