#include "daemon.h"
#include "bms.h"
#include "fields.h"
#include "mqtt.h"
#include "serial.h"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <poll.h>
#include <unistd.h>
#include <utility>

namespace pace {

namespace {

using Clock = std::chrono::steady_clock;

volatile sig_atomic_t g_stop = 0;
int g_wake_pipe[2] = {-1, -1};

void on_signal(int) {
    g_stop = 1;
    int saved = errno;
    if (g_wake_pipe[1] >= 0) {
        char c = 1;
        ssize_t r = ::write(g_wake_pipe[1], &c, 1);
        (void)r;
    }
    errno = saved;
}

// Info to stdout, errors to stderr; both flushed per line so journald sees
// them immediately when stdout is a pipe.
void log_info(const std::string& msg) { std::cout << msg << std::endl; }
void log_err (const std::string& msg) { std::cerr << msg << std::endl; }

// Logs a failure once per distinct message and a single line on recovery,
// so a persistent fault does not print every cycle.
class Problem {
public:
    explicit Problem(std::string what) : what_(std::move(what)) {}
    void fail(const std::string& msg) {
        ++count_;
        if (msg != last_) {
            log_err(what_ + ": " + msg + (count_ == 1 ? "" : " (repeats suppressed)"));
            last_ = msg;
        }
    }
    // Returns true if this was a recovery from one or more failures.
    bool ok(bool announce = true) {
        bool was_failing = count_ > 0;
        if (was_failing && announce)
            log_info(what_ + ": ok again after " + std::to_string(count_) + " failure(s)");
        count_ = 0;
        last_.clear();
        return was_failing;
    }
private:
    std::string what_;
    std::string last_;
    long        count_ = 0;
};

// Keys the CLI prints that are not published.
bool is_published(const std::string& key) {
    static const char* const kExcluded[] = {
        "pack", "cell_count", "temp_count", "current_a", "total_voltage_mv",
        "remain_cap_mah", "full_cap_mah", "design_cap_mah",
    };
    for (const char* k : kExcluded)
        if (key == k) return false;
    return true;
}

std::string make_client_id() {
    char host[256] = {};
    if (::gethostname(host, sizeof(host) - 1) != 0) host[0] = '\0';
    std::string id = "pbmstool-";
    for (const char* p = host; *p && *p != '.'; ++p) {
        char c = *p;
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-';
        if (ok) id += c;
    }
    if (id.size() > 23) id.resize(23);  // MQTT 3.1.1 servers must accept up to 23 bytes
    return id;
}

bool make_wake_pipe() {
    if (::pipe(g_wake_pipe) != 0) return false;
    for (int fd : g_wake_pipe) {
        int fl = fcntl(fd, F_GETFL, 0);
        if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
        fcntl(fd, F_SETFD, FD_CLOEXEC);
    }
    return true;
}

std::string fmt_seconds(double s) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%g", s);
    return buf;
}

} // namespace

int run_daemon(const DaemonConfig& cfg) {
    // SIGPIPE: belt and braces alongside MSG_NOSIGNAL / SO_NOSIGPIPE in mqtt.cpp.
    std::signal(SIGPIPE, SIG_IGN);
    if (!make_wake_pipe()) {
        log_err(std::string("daemon: pipe: ") + std::strerror(errno));
        return 1;
    }
    struct sigaction sa {};
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;  // no SA_RESTART: interrupt blocking poll/read promptly
    sigaction(SIGINT,  &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    std::string prefix = cfg.mqtt_prefix;
    while (!prefix.empty() && prefix.back() == '/') prefix.pop_back();
    if (!prefix.empty()) prefix += '/';

    const std::string broker    = cfg.mqtt_host + ":" + std::to_string(cfg.mqtt_port);
    const std::string client_id = make_client_id();

    Serial serial;
    BMS    bms(serial);
    bool   serial_open  = false;
    bool   ever_opened  = false;
    Problem serial_p("serial: cannot open " + cfg.port);
    Problem analog_p("analog");
    Problem alarm_p("alarm");

    // One BMS read blocks for up to kMaxAttempts (3) serial timeouts, during which
    // no PINGREQ can be sent. PINGREQ goes out every keepalive/2, so keepalive
    // must be at least twice the longest read to stay within the broker's limit.
    const int vtime_s    = (std::max(1, std::min(255, (cfg.timeout_ms + 99) / 100)) + 9) / 10;
    const int keepalive  = std::max(30, 2 * 3 * vtime_s + 10);
    MqttClient mqtt(cfg.mqtt_host, cfg.mqtt_port, client_id, keepalive);
    Problem    mqtt_p("mqtt: connect to " + broker);
    log_info("pbmstool daemon: port=" + cfg.port + " baud=" + std::to_string(cfg.baud) +
             " timeout=" + std::to_string(cfg.timeout_ms) + "ms pack=" +
             std::to_string(static_cast<int>(cfg.pack)) + " interval=" +
             fmt_seconds(cfg.interval_s) + "s mqtt=" + broker + " topics=" + prefix +
             "<field> client_id=" + client_id + " keepalive=" + std::to_string(keepalive) + "s");

    const auto kBackoffMin = std::chrono::seconds(1);
    const auto kBackoffMax = std::chrono::seconds(60);
    const auto kStableConn = std::chrono::seconds(60);
    auto backoff        = std::chrono::seconds(kBackoffMin);
    auto next_mqtt_try  = Clock::now();
    auto connected_at   = Clock::now();
    bool announced      = false;

    auto on_mqtt_lost = [&](const std::string& err) {
        log_err("mqtt: connection to " + broker + " lost: " + err);
        auto now = Clock::now();
        // A connection that drops right after connecting keeps backing off.
        if (now - connected_at >= kStableConn) backoff = kBackoffMin;
        next_mqtt_try = now + backoff;
        backoff = std::min(backoff * 2, std::chrono::seconds(kBackoffMax));
    };

    // Non-blocking keepalive upkeep, also called between the two BMS reads.
    auto service_mqtt = [&]() {
        std::string err;
        if (mqtt.connected() && !mqtt.service(err)) on_mqtt_lost(err);
    };

    auto run_cycle = [&]() {
        std::string err;
        if (!serial_open) {
            if (!serial.open(cfg.port, cfg.baud, cfg.timeout_ms)) {
                serial_p.fail(std::strerror(errno));
                return;
            }
            serial_open = true;
            if (serial_p.ok(false) || !ever_opened) log_info("serial: opened " + cfg.port);
            ever_opened = true;
        }

        KeyValues fields;
        bool any_failed = false;

        AnalogData a;
        if (bms.get_analog(cfg.pack, a, err)) {
            analog_p.ok();
            for (auto& kv : analog_fields(cfg.pack, a))
                if (is_published(kv.first)) fields.push_back(std::move(kv));
        } else {
            any_failed = true;
            if (!g_stop) analog_p.fail(err);
        }
        if (g_stop) return;
        service_mqtt();

        AlarmData al;
        if (bms.get_alarm(cfg.pack, al, err)) {
            alarm_p.ok();
            for (auto& kv : alarm_fields(cfg.pack, al))
                if (is_published(kv.first)) fields.push_back(std::move(kv));
        } else {
            any_failed = true;
            if (!g_stop) alarm_p.fail(err);
        }

        // Reopen the device next cycle: recovers from a replugged USB adapter
        // and releases the old device node so it can reappear under the same name.
        if (any_failed) { serial.close(); serial_open = false; }

        if (fields.empty() || !mqtt.connected() || g_stop) return;
        MqttClient::Messages msgs;
        msgs.reserve(fields.size());
        for (auto& kv : fields) msgs.emplace_back(prefix + kv.first, std::move(kv.second));
        if (!mqtt.publish(msgs, err)) { on_mqtt_lost(err); return; }
        if (!announced) {
            log_info("mqtt: publishing " + std::to_string(msgs.size()) + " topics");
            announced = true;
        }
    };

    const auto interval = std::chrono::duration_cast<Clock::duration>(
        std::chrono::duration<double>(cfg.interval_s));
    auto next_cycle = Clock::now();

    while (!g_stop) {
        std::string err;

        if (!mqtt.connected() && Clock::now() >= next_mqtt_try) {
            if (mqtt.connect(err)) {
                mqtt_p.ok(false);
                log_info("mqtt: connected to " + broker);
                connected_at = Clock::now();
                announced = false;
            } else {
                if (g_stop) break;
                mqtt_p.fail(err);
                next_mqtt_try = Clock::now() + backoff;
                backoff = std::min(backoff * 2, std::chrono::seconds(kBackoffMax));
            }
        }
        service_mqtt();

        if (Clock::now() >= next_cycle) {
            run_cycle();
            // Fixed cadence from the monotonic clock; after an overrun, skip the
            // missed slots instead of running them back to back.
            next_cycle += interval;
            auto now = Clock::now();
            if (next_cycle <= now) {
                auto missed = (now - next_cycle) / interval + 1;
                next_cycle += missed * interval;
            }
            continue;
        }

        auto wake = next_cycle;
        wake = std::min(wake, mqtt.connected() ? mqtt.next_deadline() : next_mqtt_try);
        auto wait = std::chrono::ceil<std::chrono::milliseconds>(wake - Clock::now()).count();
        int timeout_ms = static_cast<int>(std::max<long long>(0, std::min<long long>(wait, 60000)));

        pollfd fds[2] = {{g_wake_pipe[0], POLLIN, 0}, {mqtt.fd(), POLLIN, 0}};
        nfds_t nfds = mqtt.connected() ? 2 : 1;
        int r = ::poll(fds, nfds, timeout_ms);
        if (r < 0) {
            if (errno != EINTR) { log_err(std::string("daemon: poll: ") + std::strerror(errno)); ::usleep(100000); }
            continue;
        }
        if (nfds == 2 && (fds[1].revents & (POLLIN | POLLHUP | POLLERR))) {
            if (!mqtt.handle_readable(err)) on_mqtt_lost(err);
        }
    }

    log_info("pbmstool daemon: shutting down");
    mqtt.disconnect();
    serial.close();
    return 0;
}

} // namespace pace
