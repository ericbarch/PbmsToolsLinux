#pragma once
#include <cstdint>
#include <string>

namespace pace {

struct DaemonConfig {
    std::string port;                 // serial device
    int         baud        = 9600;
    int         timeout_ms  = 2000;
    uint8_t     pack        = 1;
    double      interval_s  = 5.0;    // read + publish cadence
    std::string mqtt_host;            // required
    int         mqtt_port   = 1883;
    std::string mqtt_prefix = "bms";  // topic = <prefix>/<field>; empty = <field>
};

// Run the foreground read-and-publish loop until SIGINT/SIGTERM. Returns the
// process exit code (0 on a signal-initiated shutdown).
int run_daemon(const DaemonConfig& cfg);

} // namespace pace
