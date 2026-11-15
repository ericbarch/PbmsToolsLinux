#include "mqtt.h"
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

namespace pace {

namespace {

// Linux suppresses SIGPIPE per call with MSG_NOSIGNAL; macOS/BSD lack that flag
// and use the SO_NOSIGPIPE socket option instead (set in connect()).
#ifdef MSG_NOSIGNAL
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0;
#endif

enum : uint8_t {
    PKT_CONNECT    = 0x10,
    PKT_CONNACK    = 2,
    PKT_PUBLISH    = 0x30,  // QoS 0, no DUP, no RETAIN
    PKT_PINGREQ    = 0xC0,
    PKT_PINGRESP   = 13,
    PKT_DISCONNECT = 0xE0,
};

void put_remaining_length(std::vector<uint8_t>& out, size_t len) {
    do {
        uint8_t b = static_cast<uint8_t>(len % 128);
        len /= 128;
        if (len > 0) b |= 0x80;
        out.push_back(b);
    } while (len > 0);
}

void put_string(std::vector<uint8_t>& out, const std::string& s) {
    out.push_back(static_cast<uint8_t>((s.size() >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(s.size() & 0xFF));
    out.insert(out.end(), s.begin(), s.end());
}

const char* connack_reason(uint8_t rc) {
    switch (rc) {
        case 1: return "unacceptable protocol version";
        case 2: return "client identifier rejected";
        case 3: return "server unavailable";
        case 4: return "bad user name or password";
        case 5: return "not authorized";
        default: return "unknown reason";
    }
}

int ms_until(MqttClient::Clock::time_point deadline) {
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  deadline - MqttClient::Clock::now()).count();
    return ms < 0 ? 0 : static_cast<int>(ms);
}

// Non-blocking TCP connect to one resolved address, bounded by timeout_ms.
int connect_one(const addrinfo* ai, int timeout_ms, std::string& err) {
    int fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (fd < 0) { err = std::strerror(errno); return -1; }
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0 || fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0) {
        err = std::strerror(errno); ::close(fd); return -1;
    }
    fcntl(fd, F_SETFD, FD_CLOEXEC);
#ifdef SO_NOSIGPIPE
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
    if (::connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) return fd;
    if (errno != EINPROGRESS) { err = std::strerror(errno); ::close(fd); return -1; }

    pollfd p{fd, POLLOUT, 0};
    int r = ::poll(&p, 1, timeout_ms);
    if (r < 0) { err = (errno == EINTR) ? "interrupted" : std::strerror(errno); ::close(fd); return -1; }
    if (r == 0) { err = "connect timed out"; ::close(fd); return -1; }
    int soerr = 0;
    socklen_t len = sizeof(soerr);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &len) < 0) soerr = errno;
    if (soerr != 0) { err = std::strerror(soerr); ::close(fd); return -1; }
    return fd;
}

} // namespace

MqttClient::MqttClient(std::string host, int port, std::string client_id, int keepalive_s)
    : host_(std::move(host)), port_(port), client_id_(std::move(client_id)),
      keepalive_s_(keepalive_s < 2 ? 2 : keepalive_s) {}

MqttClient::~MqttClient() { close_socket(); }

void MqttClient::close_socket() {
    if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
    in_.clear();
    got_connack_ = false;
    ping_outstanding_ = false;
}

bool MqttClient::connect(std::string& err) {
    close_socket();

    addrinfo hints{};
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    std::string port_str = std::to_string(port_);
    int gai = ::getaddrinfo(host_.c_str(), port_str.c_str(), &hints, &res);
    if (gai != 0) { err = std::string("cannot resolve host: ") + gai_strerror(gai); return false; }

    std::string last_err = "no addresses";
    for (addrinfo* ai = res; ai != nullptr; ai = ai->ai_next) {
        fd_ = connect_one(ai, kIoTimeoutMs, last_err);
        if (fd_ >= 0 || last_err == "interrupted") break;
    }
    ::freeaddrinfo(res);
    if (fd_ < 0) { err = last_err; return false; }

    // CONNECT: protocol "MQTT" level 4, flags = clean session, keepalive, client id.
    std::vector<uint8_t> body;
    put_string(body, "MQTT");
    body.push_back(4);
    body.push_back(0x02);
    body.push_back(static_cast<uint8_t>((keepalive_s_ >> 8) & 0xFF));
    body.push_back(static_cast<uint8_t>(keepalive_s_ & 0xFF));
    put_string(body, client_id_);
    std::vector<uint8_t> pkt{PKT_CONNECT};
    put_remaining_length(pkt, body.size());
    pkt.insert(pkt.end(), body.begin(), body.end());
    if (!send_all(pkt, err)) { close_socket(); return false; }

    auto deadline = Clock::now() + std::chrono::milliseconds(kIoTimeoutMs);
    while (!got_connack_) {
        pollfd p{fd_, POLLIN, 0};
        int r = ::poll(&p, 1, ms_until(deadline));
        if (r < 0) {
            err = (errno == EINTR) ? "interrupted" : std::strerror(errno);
            close_socket(); return false;
        }
        if (r == 0) { err = "no CONNACK from broker"; close_socket(); return false; }
        if (!read_available(err) || !parse_packets(err)) { close_socket(); return false; }
    }
    if (connack_rc_ != 0) {
        err = "broker refused connection (rc=" + std::to_string(connack_rc_) + ": " +
              connack_reason(connack_rc_) + ")";
        close_socket();
        return false;
    }
    last_ping_ = Clock::now();
    ping_outstanding_ = false;
    return true;
}

void MqttClient::disconnect() {
    if (fd_ < 0) return;
    const uint8_t pkt[2] = {PKT_DISCONNECT, 0};
    (void)::send(fd_, pkt, sizeof(pkt), kSendFlags);
    ::shutdown(fd_, SHUT_RDWR);
    close_socket();
}

bool MqttClient::send_all(const std::vector<uint8_t>& buf, std::string& err) {
    if (fd_ < 0) { err = "not connected"; return false; }
    size_t off = 0;
    auto deadline = Clock::now() + std::chrono::milliseconds(kIoTimeoutMs);
    while (off < buf.size()) {
        ssize_t n = ::send(fd_, buf.data() + off, buf.size() - off, kSendFlags);
        if (n > 0) { off += static_cast<size_t>(n); continue; }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            int wait = ms_until(deadline);
            if (wait == 0) { err = "send timed out"; return false; }
            pollfd p{fd_, POLLOUT, 0};
            if (::poll(&p, 1, wait) < 0 && errno != EINTR) { err = std::strerror(errno); return false; }
            continue;
        }
        err = (n == 0) ? "send failed" : std::strerror(errno);
        return false;
    }
    return true;
}

bool MqttClient::read_available(std::string& err) {
    uint8_t buf[512];
    for (;;) {
        ssize_t n = ::recv(fd_, buf, sizeof(buf), 0);
        if (n > 0) { in_.insert(in_.end(), buf, buf + n); continue; }
        if (n == 0) { err = "connection closed by broker"; return false; }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return true;
        err = std::strerror(errno);
        return false;
    }
}

bool MqttClient::parse_packets(std::string& err) {
    size_t pos = 0;
    while (in_.size() - pos >= 2) {
        // Fixed header: type/flags byte + 1..4 byte remaining length.
        size_t len = 0, mult = 1, i = pos + 1;
        bool complete = false;
        for (int k = 0; k < 4 && i < in_.size(); ++k, ++i) {
            len += (in_[i] & 0x7F) * mult;
            mult *= 128;
            if (!(in_[i] & 0x80)) { complete = true; ++i; break; }
        }
        if (!complete) {
            if (i - (pos + 1) >= 4) { err = "malformed packet from broker"; return false; }
            break;  // need more bytes
        }
        if (len > 1u << 20) { err = "oversized packet from broker"; return false; }
        if (in_.size() - i < len) break;  // need more bytes

        uint8_t type = in_[pos] >> 4;
        if (type == PKT_CONNACK && len >= 2) {
            got_connack_ = true;
            connack_rc_  = in_[i + 1];
        } else if (type == PKT_PINGRESP) {
            ping_outstanding_ = false;
        }
        // Anything else is unexpected for a publish-only client; ignore it.
        pos = i + len;
    }
    in_.erase(in_.begin(), in_.begin() + static_cast<std::ptrdiff_t>(pos));
    return true;
}

bool MqttClient::publish(const Messages& msgs, std::string& err) {
    std::vector<uint8_t> out;
    std::vector<uint8_t> body;
    for (const auto& m : msgs) {
        if (m.first.size() > 0xFFFF) { err = "topic too long: " + m.first; return false; }
        body.clear();
        put_string(body, m.first);
        body.insert(body.end(), m.second.begin(), m.second.end());
        out.push_back(PKT_PUBLISH);
        put_remaining_length(out, body.size());
        out.insert(out.end(), body.begin(), body.end());
    }
    if (out.empty()) return true;
    if (!send_all(out, err)) { close_socket(); return false; }
    return true;
}

bool MqttClient::handle_readable(std::string& err) {
    if (fd_ < 0) { err = "not connected"; return false; }
    if (!read_available(err) || !parse_packets(err)) { close_socket(); return false; }
    return true;
}

MqttClient::Clock::time_point MqttClient::next_deadline() const {
    return last_ping_ + std::chrono::seconds(keepalive_s_ / 2);
}

bool MqttClient::service(std::string& err) {
    if (fd_ < 0) { err = "not connected"; return false; }
    // Pick up a PINGRESP that arrived while the caller was blocked elsewhere
    // (e.g. a slow serial read) before judging the connection dead.
    if (!read_available(err) || !parse_packets(err)) { close_socket(); return false; }
    auto now = Clock::now();
    if (now < next_deadline()) return true;
    if (ping_outstanding_) {
        err = "no PINGRESP within " + std::to_string(keepalive_s_ / 2) + "s";
        close_socket();
        return false;
    }
    const std::vector<uint8_t> ping{PKT_PINGREQ, 0};
    if (!send_all(ping, err)) { close_socket(); return false; }
    last_ping_ = now;
    ping_outstanding_ = true;
    return true;
}

} // namespace pace
