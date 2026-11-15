#pragma once
#include <chrono>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace pace {

// Minimal MQTT 3.1.1 client over plain TCP: clean session, no authentication,
// QoS 0 PUBLISH only (not retained), PINGREQ keepalive, no subscriptions.
// Single-threaded and non-blocking after connect: the owner polls fd() for
// POLLIN, calls handle_readable() when it fires, and calls service() at or
// after next_deadline(). Any failure closes the socket; connected() then
// returns false and the owner decides when to call connect() again.
class MqttClient {
public:
    using Clock    = std::chrono::steady_clock;
    using Messages = std::vector<std::pair<std::string, std::string>>;  // topic, payload

    MqttClient(std::string host, int port, std::string client_id, int keepalive_s = 30);
    ~MqttClient();
    MqttClient(const MqttClient&)            = delete;
    MqttClient& operator=(const MqttClient&) = delete;

    // Resolve host (getaddrinfo), TCP connect, send CONNECT, wait for CONNACK.
    // Blocks for at most ~2 * kIoTimeoutMs. Returns false with err set on failure,
    // including err = "interrupted" if a signal arrives while waiting.
    bool connect(std::string& err);

    // Send DISCONNECT (best effort) and close the socket.
    void disconnect();

    bool connected() const { return fd_ >= 0; }
    int  fd() const { return fd_; }

    // Publish all messages at QoS 0 in a single write.
    bool publish(const Messages& msgs, std::string& err);

    // Drain inbound bytes (PINGRESP etc.). False if the broker closed the connection.
    bool handle_readable(std::string& err);

    // Send PINGREQ when due; fail if the previous PINGREQ went unanswered.
    bool service(std::string& err);

    // When service() next needs to run.
    Clock::time_point next_deadline() const;

private:
    static constexpr int kIoTimeoutMs = 5000;

    void close_socket();
    bool send_all(const std::vector<uint8_t>& buf, std::string& err);
    bool read_available(std::string& err);
    bool parse_packets(std::string& err);

    std::string host_;
    int         port_;
    std::string client_id_;
    int         keepalive_s_;

    int fd_ = -1;
    std::vector<uint8_t> in_;
    bool    got_connack_ = false;
    uint8_t connack_rc_  = 0;
    bool    ping_outstanding_ = false;
    Clock::time_point last_ping_;
};

} // namespace pace
