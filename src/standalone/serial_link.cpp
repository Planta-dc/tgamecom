#include "serial_link.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace standalone {

namespace {

constexpr uint8_t kFrameSync = 0xA5u;
constexpr uint8_t kCmdData   = 0x01u;
constexpr uint8_t kCmdLines  = 0x02u;

void ensure_sockets_init()
{
#if defined(_WIN32)
    static bool initialized = false;
    if (!initialized) {
        WSADATA wsa{};
        (void)WSAStartup(MAKEWORD(2, 2), &wsa);
        initialized = true;
    }
#endif
}

void close_fd(int& fd) noexcept
{
    if (fd < 0) return;
#if defined(_WIN32)
    closesocket(static_cast<SOCKET>(fd));
#else
    ::close(fd);
#endif
    fd = -1;
}

bool set_nonblocking_nodelay(int fd, bool is_stream_peer)
{
#if defined(_WIN32)
    u_long mode = 1;
    if (ioctlsocket(static_cast<SOCKET>(fd), FIONBIO, &mode) != 0) return false;
    if (is_stream_peer) {
        BOOL one = TRUE;
        (void)setsockopt(static_cast<SOCKET>(fd), IPPROTO_TCP, TCP_NODELAY,
                         reinterpret_cast<const char *>(&one), sizeof(one));
    }
    return true;
#else
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) return false;
    if (is_stream_peer) {
        const int one = 1;
        (void)::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    }
    return true;
#endif
}

bool socket_would_block() noexcept
{
#if defined(_WIN32)
    const int err = WSAGetLastError();
    return err == WSAEWOULDBLOCK || err == WSAEINPROGRESS;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINPROGRESS;
#endif
}

int send_nonblock(int fd, const void *buf, std::size_t len) noexcept
{
#if defined(_WIN32)
    return ::send(static_cast<SOCKET>(fd), static_cast<const char *>(buf),
                  static_cast<int>(len), 0);
#else
    int flags = 0;
#ifdef MSG_NOSIGNAL
    flags |= MSG_NOSIGNAL;
#endif
    return static_cast<int>(::send(fd, buf, len, flags));
#endif
}

int recv_nonblock(int fd, void *buf, std::size_t len) noexcept
{
#if defined(_WIN32)
    return ::recv(static_cast<SOCKET>(fd), static_cast<char *>(buf),
                  static_cast<int>(len), 0);
#else
    return static_cast<int>(::recv(fd, buf, len, 0));
#endif
}

std::string trim_copy(const std::string& in)
{
    std::size_t b = 0;
    std::size_t e = in.size();
    while (b < e && std::isspace(static_cast<unsigned char>(in[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(in[e - 1]))) --e;
    return in.substr(b, e - b);
}

std::string digits_or_token(const std::string& in)
{
    std::string trimmed = trim_copy(in);
    if (trimmed.find('.') != std::string::npos || trimmed.find(':') != std::string::npos)
        return trimmed;
    std::string clean;
    for (char c : trimmed) {
        if (!std::isspace(static_cast<unsigned char>(c)) && c != '-' && c != '(' && c != ')')
            clean.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return clean;
}

} // namespace

bool parse_host_port(const std::string& spec, std::string& out_host,
                     uint16_t& out_port, uint16_t default_port)
{
    const std::string s = trim_copy(spec);
    if (s.empty()) return false;
    const std::size_t colon = s.rfind(':');
    if (colon == std::string::npos) {
        out_host = s;
        out_port = default_port;
        return !out_host.empty();
    }
    out_host = trim_copy(s.substr(0, colon));
    const std::string port_str = trim_copy(s.substr(colon + 1));
    if (out_host.empty()) out_host = "127.0.0.1";
    char *end = nullptr;
    const unsigned long p = std::strtoul(port_str.c_str(), &end, 10);
    if (!end || end == port_str.c_str() || *end != '\0' || p == 0ul || p > 65535ul)
        return false;
    out_port = static_cast<uint16_t>(p);
    return true;
}

SerialLink::~SerialLink()
{
    close_sockets();
}

void SerialLink::close_sockets() noexcept
{
    close_fd(m_conn_fd);
    close_fd(m_listen_fd);
    m_bound_port = 0;
    m_rx_frame_len = 0;
    m_sent_initial_lines = false;
}

void SerialLink::disconnect()
{
    close_sockets();
    m_mode = Mode::Disconnected;
    m_peer_machine = nullptr;
    m_peer_cpu = nullptr;
    m_rx_pending.clear();
    m_modem_online = false;
    m_modem_portal_active = false;
    m_cmd_line.clear();
    m_portal_line.clear();
}

bool SerialLink::is_connected() const noexcept
{
    switch (m_mode) {
    case Mode::Disconnected: return false;
    case Mode::PeerCrossover: return m_peer_machine != nullptr && m_peer_cpu != nullptr;
    case Mode::TcpHost:
    case Mode::TcpClient: return m_conn_fd >= 0;
    case Mode::VirtualModem: return true;
    }
    return false;
}

std::string SerialLink::status_summary() const
{
    char buf[128];
    switch (m_mode) {
    case Mode::Disconnected:
        return "SERIAL OFF";
    case Mode::PeerCrossover:
        return "2P LOCAL CROSSOVER";
    case Mode::TcpHost:
        if (m_conn_fd >= 0)
            std::snprintf(buf, sizeof(buf), "2P HOST :%u (LINKED)", m_bound_port);
        else
            std::snprintf(buf, sizeof(buf), "2P HOST :%u (WAITING)", m_bound_port);
        return buf;
    case Mode::TcpClient:
        if (m_conn_fd >= 0)
            std::snprintf(buf, sizeof(buf), "2P CLIENT %s:%u (LINKED)",
                          m_target_host.c_str(), m_target_port);
        else
            std::snprintf(buf, sizeof(buf), "2P CLIENT %s:%u (OFFLINE)",
                          m_target_host.c_str(), m_target_port);
        return buf;
    case Mode::VirtualModem:
        if (m_modem_online)
            return m_modem_portal_active ? "MODEM ONLINE (PORTAL)" : "MODEM ONLINE (TCP)";
        return "MODEM READY (9600 AT)";
    }
    return "SERIAL OFF";
}

void SerialLink::attach_peer(GcMachine& peer_machine, Sm8521Cpu& peer_cpu)
{
    disconnect();
    m_mode = Mode::PeerCrossover;
    m_peer_machine = &peer_machine;
    m_peer_cpu = &peer_cpu;
}

void SerialLink::step_crossover_pair(GcMachine& a_machine, Sm8521Cpu& a_cpu,
                                     GcMachine& b_machine, Sm8521Cpu& b_cpu)
{
    /* Null-modem Compete.com cable: DTR <-> DSR, RTS <-> CTS, TXD <-> RXD. */
    gc_serial_set_inputs(&a_machine, gc_serial_rts(&b_machine), gc_serial_dtr(&b_machine));
    gc_serial_set_inputs(&b_machine, gc_serial_rts(&a_machine), gc_serial_dtr(&a_machine));

    for (const auto& tx : a_cpu.uart_transmits())
        (void)b_cpu.queue_uart_receive(tx.value);
    a_cpu.clear_uart_transmits();

    for (const auto& tx : b_cpu.uart_transmits())
        (void)a_cpu.queue_uart_receive(tx.value);
    b_cpu.clear_uart_transmits();
}

bool SerialLink::listen_tcp(uint16_t port, std::string *error)
{
    disconnect();
    ensure_sockets_init();

    const int fd = static_cast<int>(::socket(AF_INET, SOCK_STREAM, 0));
    if (fd < 0) {
        if (error) *error = "could not create TCP listen socket";
        return false;
    }
    int reuse = 1;
#if defined(_WIN32)
    (void)setsockopt(static_cast<SOCKET>(fd), SOL_SOCKET, SO_REUSEADDR,
                     reinterpret_cast<const char *>(&reuse), sizeof(reuse));
#else
    (void)::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#endif

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (::bind(fd, reinterpret_cast<const sockaddr *>(&addr), sizeof(addr)) != 0 ||
        ::listen(fd, 1) != 0 || !set_nonblocking_nodelay(fd, false)) {
        int tmp = fd;
        close_fd(tmp);
        if (error) *error = "could not bind/listen on requested TCP port";
        return false;
    }

    sockaddr_in bound{};
#if defined(_WIN32)
    int bound_len = sizeof(bound);
#else
    socklen_t bound_len = sizeof(bound);
#endif
    if (::getsockname(fd, reinterpret_cast<sockaddr *>(&bound), &bound_len) == 0)
        m_bound_port = ntohs(bound.sin_port);
    else
        m_bound_port = port;

    m_listen_fd = fd;
    m_mode = Mode::TcpHost;
    m_remote_dtr = true;
    m_remote_rts = true;
    return true;
}

bool SerialLink::open_tcp_connection(const std::string& host, uint16_t port, std::string *error)
{
    ensure_sockets_init();
    const std::string resolved_host = (host == "localhost") ? "127.0.0.1" : host;

    sockaddr_in ipv4{};
    ipv4.sin_family = AF_INET;
    ipv4.sin_port = htons(port);
    if (::inet_pton(AF_INET, resolved_host.c_str(), &ipv4.sin_addr) == 1) {
        int fd = static_cast<int>(::socket(AF_INET, SOCK_STREAM, 0));
        if (fd >= 0 && set_nonblocking_nodelay(fd, true)) {
            const int rc = ::connect(fd, reinterpret_cast<const sockaddr *>(&ipv4), sizeof(ipv4));
            if (rc == 0 || socket_would_block()) {
                m_conn_fd = fd;
                return true;
            }
        }
        close_fd(fd);
        if (error) *error = "could not connect to " + host + ":" + std::to_string(port);
        return false;
    }

#if !defined(GAMECOM_STATIC_NO_DNS)
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    char port_buf[16];
    std::snprintf(port_buf, sizeof(port_buf), "%u", static_cast<unsigned>(port));

    addrinfo *res = nullptr;
    if (::getaddrinfo(resolved_host.c_str(), port_buf, &hints, &res) != 0 || !res) {
        if (error) *error = "could not resolve host: " + host;
        return false;
    }

    int fd = -1;
    for (addrinfo *it = res; it != nullptr; it = it->ai_next) {
        fd = static_cast<int>(::socket(it->ai_family, it->ai_socktype, it->ai_protocol));
        if (fd < 0) continue;
        if (!set_nonblocking_nodelay(fd, true)) {
            close_fd(fd);
            continue;
        }
        const int rc = ::connect(fd, it->ai_addr, static_cast<socklen_t>(it->ai_addrlen));
        if (rc == 0 || socket_would_block()) break;
        close_fd(fd);
    }
    ::freeaddrinfo(res);

    if (fd >= 0) {
        m_conn_fd = fd;
        return true;
    }
#endif
    if (error) *error = "could not connect to " + host + ":" + std::to_string(port);
    return false;
}

bool SerialLink::connect_tcp(const std::string& host, uint16_t port, std::string *error)
{
    disconnect();
    m_target_host = host;
    m_target_port = port;
    m_remote_dtr = true;
    m_remote_rts = true;
    if (!open_tcp_connection(host, port, error)) return false;
    m_mode = Mode::TcpClient;
    return true;
}

void SerialLink::enable_virtual_modem()
{
    disconnect();
    m_mode = Mode::VirtualModem;
    modem_reset_defaults();
}

void SerialLink::modem_reset_defaults()
{
    m_modem_echo = true;
    m_modem_quiet = false;
    m_modem_verbose = true;
    m_modem_dtr_mode = 2;
    m_modem_s0 = 0;
    m_plus_count = 0;
    m_cmd_line.clear();
    m_portal_line.clear();
}

bool SerialLink::load_phonebook(const std::string& path, std::string *error)
{
    std::ifstream in(path);
    if (!in) {
        if (error) *error = "could not open phonebook file";
        return false;
    }
    std::string line;
    while (std::getline(in, line)) {
        const std::size_t hash = line.find('#');
        if (hash != std::string::npos) line.erase(hash);
        const std::size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = digits_or_token(line.substr(0, eq));
        const std::string val = trim_copy(line.substr(eq + 1));
        if (!key.empty() && !val.empty()) m_phonebook[key] = val;
    }
    return true;
}

void SerialLink::set_phonebook_entry(const std::string& number, const std::string& target)
{
    const std::string key = digits_or_token(number);
    if (!key.empty()) m_phonebook[key] = trim_copy(target);
}

void SerialLink::poll(GcMachine& machine, Sm8521Cpu& cpu)
{
    switch (m_mode) {
    case Mode::Disconnected:
        gc_serial_set_inputs(&machine, true, true);
        cpu.clear_uart_transmits();
        break;
    case Mode::PeerCrossover:
        if (m_peer_machine && m_peer_cpu)
            step_crossover_pair(machine, cpu, *m_peer_machine, *m_peer_cpu);
        break;
    case Mode::TcpHost:
    case Mode::TcpClient:
        poll_tcp_link(machine, cpu);
        break;
    case Mode::VirtualModem:
        poll_modem(machine, cpu);
        break;
    }
}

void SerialLink::poll_tcp_link(GcMachine& machine, Sm8521Cpu& cpu)
{
    if (m_mode == Mode::TcpHost && m_conn_fd < 0 && m_listen_fd >= 0) {
        sockaddr_in peer{};
#if defined(_WIN32)
        int peer_len = sizeof(peer);
#else
        socklen_t peer_len = sizeof(peer);
#endif
        const int accepted = static_cast<int>(
            ::accept(m_listen_fd, reinterpret_cast<sockaddr *>(&peer), &peer_len));
        if (accepted >= 0) {
            (void)set_nonblocking_nodelay(accepted, true);
            m_conn_fd = accepted;
            m_rx_frame_len = 0;
            m_sent_initial_lines = false;
        }
    }

    if (m_conn_fd < 0) {
        gc_serial_set_inputs(&machine, true, true);
        cpu.clear_uart_transmits();
        return;
    }

    const bool local_dtr = gc_serial_dtr(&machine);
    const bool local_rts = gc_serial_rts(&machine);
    if (!m_sent_initial_lines || local_dtr != m_last_sent_dtr || local_rts != m_last_sent_rts) {
        const uint8_t pkt[3] = {
            kFrameSync, kCmdLines,
            static_cast<uint8_t>((local_dtr ? 0x01u : 0x00u) | (local_rts ? 0x02u : 0x00u))
        };
        if (send_nonblock(m_conn_fd, pkt, sizeof(pkt)) == 3) {
            m_last_sent_dtr = local_dtr;
            m_last_sent_rts = local_rts;
            m_sent_initial_lines = true;
        }
    }

    for (const auto& tx : cpu.uart_transmits()) {
        const uint8_t pkt[3] = { kFrameSync, kCmdData, tx.value };
        (void)send_nonblock(m_conn_fd, pkt, sizeof(pkt));
    }
    cpu.clear_uart_transmits();

    uint8_t buf[256];
    for (;;) {
        const int n = recv_nonblock(m_conn_fd, buf, sizeof(buf));
        if (n > 0) {
            for (int i = 0; i < n; ++i) {
                const uint8_t b = buf[i];
                if (m_rx_frame_len == 0) {
                    if (b == kFrameSync) m_rx_frame[m_rx_frame_len++] = b;
                } else {
                    m_rx_frame[m_rx_frame_len++] = b;
                    if (m_rx_frame_len == 3) {
                        if (m_rx_frame[1] == kCmdData) {
                            m_rx_pending.push_back(m_rx_frame[2]);
                        } else if (m_rx_frame[1] == kCmdLines) {
                            m_remote_dtr = (m_rx_frame[2] & 0x01u) != 0;
                            m_remote_rts = (m_rx_frame[2] & 0x02u) != 0;
                        }
                        m_rx_frame_len = 0;
                    }
                }
            }
        } else if (n == 0 || !socket_would_block()) {
            close_fd(m_conn_fd);
            m_remote_dtr = true;
            m_remote_rts = true;
            m_sent_initial_lines = false;
            break;
        } else {
            break;
        }
    }

    gc_serial_set_inputs(&machine, m_remote_rts, m_remote_dtr);
    if (!m_rx_pending.empty() && cpu.uart_rx_idle()) {
        if (cpu.queue_uart_receive(m_rx_pending.front()))
            m_rx_pending.pop_front();
    }
}

void SerialLink::modem_queue_str(const std::string& s)
{
    for (char c : s) m_rx_pending.push_back(static_cast<uint8_t>(c));
}

void SerialLink::modem_result(int numeric_code, const char *verbose_text)
{
    if (m_modem_quiet) return;
    if (m_modem_verbose) {
        modem_queue_str("\r\n");
        modem_queue_str(verbose_text);
        modem_queue_str("\r\n");
    } else {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%d\r", numeric_code);
        modem_queue_str(buf);
    }
}

void SerialLink::modem_hangup(bool emit_no_carrier)
{
    close_fd(m_conn_fd);
    const bool was_online = m_modem_online;
    m_modem_online = false;
    m_modem_portal_active = false;
    m_plus_count = 0;
    m_portal_line.clear();
    if (was_online && emit_no_carrier)
        modem_result(3, "NO CARRIER");
}

void SerialLink::portal_enter()
{
    m_modem_online = true;
    m_modem_portal_active = true;
    m_portal_line.clear();
    modem_result(1, "CONNECT 9600");
    modem_queue_str(
        "\r\n=== TIGER GAME.COM PORTAL ===\r\n"
        "1) SYSTEM STATUS\r\n"
        "2) LOCAL DATE & TIME\r\n"
        "3) DIALING & LINK HELP\r\n"
        "4) DISCONNECT\r\n"
        "SELECT [1-4]> ");
}

void SerialLink::portal_handle_byte(uint8_t ch)
{
    if (ch == '\r' || ch == '\n') {
        modem_queue_str("\r\n");
        const std::string cmd = trim_copy(m_portal_line);
        m_portal_line.clear();
        if (cmd == "1") {
            modem_queue_str(
                "CPU: SM8521 @ 5.000 MHZ\r\n"
                "LCD: 200X160 @ 75.48 HZ\r\n"
                "UART: 9600 8N1 HAYES MODEM\r\n");
        } else if (cmd == "2") {
            const std::time_t now = std::time(nullptr);
            std::tm tm_val{};
#if defined(_WIN32)
            localtime_s(&tm_val, &now);
#else
            localtime_r(&now, &tm_val);
#endif
            char tbuf[64];
            std::strftime(tbuf, sizeof(tbuf), "DATE: %Y-%m-%d %H:%M:%S\r\n", &tm_val);
            modem_queue_str(tbuf);
        } else if (cmd == "3") {
            modem_queue_str(
                "DIAL HOST:PORT VIA ATDT OR\r\n"
                "MAP NUMBERS IN PHONEBOOK:\r\n"
                "  2223334444=BBS.HOST:23\r\n");
        } else if (cmd == "4" || cmd == "quit" || cmd == "exit") {
            modem_queue_str("GOODBYE.\r\n");
            modem_hangup(true);
            return;
        } else if (!cmd.empty()) {
            modem_queue_str("ECHO: " + cmd + "\r\n");
        }
        modem_queue_str("SELECT [1-4]> ");
        return;
    }
    if (ch == 0x08u || ch == 0x7Fu) {
        if (!m_portal_line.empty()) {
            m_portal_line.pop_back();
            modem_queue_str("\b \b");
        }
        return;
    }
    if (ch >= 0x20u && ch < 0x7Fu && m_portal_line.size() < 64u) {
        m_portal_line.push_back(static_cast<char>(ch));
        m_rx_pending.push_back(ch);
    }
}

void SerialLink::modem_dial(const std::string& dial_str)
{
    std::string target = trim_copy(dial_str);
    if (!target.empty() && (target[0] == 'T' || target[0] == 't' ||
                            target[0] == 'P' || target[0] == 'p')) {
        target = trim_copy(target.substr(1));
    }
    const std::string key = digits_or_token(target);
    const auto it = m_phonebook.find(key);
    if (it != m_phonebook.end()) target = it->second;

    if (target.empty() || key == "0" || key == "5551212" ||
        key == "gamecom" || key == "portal" ||
        (target.find('.') == std::string::npos && target.find(':') == std::string::npos)) {
        portal_enter();
        return;
    }

    std::string host;
    uint16_t port = 23;
    if (!parse_host_port(target, host, port, 23)) {
        modem_result(4, "ERROR");
        return;
    }
    std::string err;
    if (!open_tcp_connection(host, port, &err)) {
        modem_result(3, "NO CARRIER");
        return;
    }
    m_modem_online = true;
    m_modem_portal_active = false;
    m_plus_count = 0;
    modem_result(1, "CONNECT 9600");
}

void SerialLink::modem_handle_command(const std::string& raw_cmd)
{
    std::string cmd = trim_copy(raw_cmd);
    if (cmd.empty()) return;

    std::string upper;
    upper.reserve(cmd.size());
    for (char c : cmd) upper.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    if (upper.size() < 2u || upper[0] != 'A' || upper[1] != 'T') {
        modem_result(4, "ERROR");
        return;
    }

    std::size_t i = 2;
    while (i < upper.size()) {
        const char c = upper[i++];
        if (std::isspace(static_cast<unsigned char>(c))) continue;
        if (c == 'Z') {
            modem_hangup(false);
            modem_reset_defaults();
        } else if (c == 'E') {
            if (i < upper.size() && (upper[i] == '0' || upper[i] == '1'))
                m_modem_echo = (upper[i++] == '1');
        } else if (c == 'Q') {
            if (i < upper.size() && (upper[i] == '0' || upper[i] == '1'))
                m_modem_quiet = (upper[i++] == '1');
        } else if (c == 'V') {
            if (i < upper.size() && (upper[i] == '0' || upper[i] == '1'))
                m_modem_verbose = (upper[i++] == '1');
        } else if (c == 'H') {
            if (i < upper.size() && std::isdigit(static_cast<unsigned char>(upper[i]))) ++i;
            modem_hangup(false);
        } else if (c == 'O') {
            if (i < upper.size() && std::isdigit(static_cast<unsigned char>(upper[i]))) ++i;
            if (m_conn_fd >= 0 || m_modem_portal_active) {
                m_modem_online = true;
                modem_result(1, "CONNECT 9600");
                return;
            }
            modem_result(3, "NO CARRIER");
            return;
        } else if (c == 'I') {
            while (i < upper.size() && std::isdigit(static_cast<unsigned char>(upper[i]))) ++i;
            modem_queue_str("\r\nTGAMECOM VIRTUAL 9600 MODEM\r\n");
        } else if (c == '&') {
            if (i < upper.size()) {
                const char sub = upper[i++];
                if (sub == 'F') modem_reset_defaults();
                else if (sub == 'D') {
                    if (i < upper.size() && std::isdigit(static_cast<unsigned char>(upper[i])))
                        m_modem_dtr_mode = static_cast<uint8_t>(upper[i++] - '0');
                } else {
                    while (i < upper.size() && std::isdigit(static_cast<unsigned char>(upper[i]))) ++i;
                }
            }
        } else if (c == 'S') {
            unsigned reg = 0;
            while (i < upper.size() && std::isdigit(static_cast<unsigned char>(upper[i])))
                reg = reg * 10u + static_cast<unsigned>(upper[i++] - '0');
            if (i < upper.size() && upper[i] == '=') {
                ++i;
                unsigned val = 0;
                while (i < upper.size() && std::isdigit(static_cast<unsigned char>(upper[i])))
                    val = val * 10u + static_cast<unsigned>(upper[i++] - '0');
                if (reg == 0u) m_modem_s0 = static_cast<uint8_t>(val & 0xFFu);
            } else if (i < upper.size() && upper[i] == '?') {
                ++i;
                char sbuf[16];
                std::snprintf(sbuf, sizeof(sbuf), "\r\n%03u\r\n",
                              reg == 0u ? static_cast<unsigned>(m_modem_s0) : 0u);
                modem_queue_str(sbuf);
            }
        } else if (c == 'D') {
            modem_dial(cmd.substr(i));
            return;
        } else {
            /* Accept benign Hayes init flags (M, L, X, W, \, %, etc.) cleanly. */
            while (i < upper.size() && (std::isdigit(static_cast<unsigned char>(upper[i])) ||
                                        upper[i] == '=')) {
                ++i;
            }
        }
    }
    modem_result(0, "OK");
}

void SerialLink::poll_modem(GcMachine& machine, Sm8521Cpu& cpu)
{
    /* Active-low RS-232 control pins on P3: 0 (false) = asserted/ready. */
    const bool cts_low = m_rx_pending.size() < 2048u;
    const bool dsr_low = true;
    gc_serial_set_inputs(&machine, !cts_low, !dsr_low);

    const bool p3_dtr_configured = (machine.bus.reg_file[0x23] & 0x02u) != 0;
    const bool dtr_low = p3_dtr_configured ? !gc_serial_dtr(&machine) : true;
    if (m_prev_dtr_low && !dtr_low && m_modem_dtr_mode >= 2u && m_modem_online)
        modem_hangup(true);
    m_prev_dtr_low = dtr_low;

    /* Guard-time check for '+++' online escape (~0.4s of silence after '+++'). */
    if (m_modem_online && m_plus_count == 3u &&
        cpu.cycles() > m_last_tx_cycle + (static_cast<uint64_t>(cpu.clock_hz()) * 2ull) / 5ull) {
        m_modem_online = false;
        m_plus_count = 0;
        modem_result(0, "OK");
    }

    for (const auto& tx : cpu.uart_transmits()) {
        const uint8_t ch = tx.value;
        if (m_modem_online) {
            if (ch == '+') {
                if (m_plus_count == 0u || tx.cycle <= m_last_tx_cycle + cpu.clock_hz() / 2u)
                    ++m_plus_count;
                else
                    m_plus_count = 1u;
            } else {
                m_plus_count = 0u;
            }
            m_last_tx_cycle = tx.cycle;

            if (m_modem_portal_active) {
                portal_handle_byte(ch);
            } else if (m_conn_fd >= 0) {
                if (send_nonblock(m_conn_fd, &ch, 1) < 0 && !socket_would_block())
                    modem_hangup(true);
            }
        } else {
            m_last_tx_cycle = tx.cycle;
            if (m_modem_echo) m_rx_pending.push_back(ch);
            if (ch == '\r' || ch == '\n') {
                if (!m_cmd_line.empty()) {
                    const std::string line = m_cmd_line;
                    m_cmd_line.clear();
                    modem_handle_command(line);
                }
            } else if (ch == 0x08u || ch == 0x7Fu) {
                if (!m_cmd_line.empty()) m_cmd_line.pop_back();
            } else if (m_cmd_line.size() < 256u) {
                m_cmd_line.push_back(static_cast<char>(ch));
            }
        }
    }
    cpu.clear_uart_transmits();

    if (m_modem_online && !m_modem_portal_active && m_conn_fd >= 0 && m_rx_pending.size() < 1024u) {
        uint8_t buf[256];
        const int n = recv_nonblock(m_conn_fd, buf, sizeof(buf));
        if (n > 0) {
            for (int i = 0; i < n; ++i) {
                /* Strip Telnet IAC 3-byte negotiations cleanly for plain TTY cartridges. */
                if (buf[i] == 0xFFu && i + 2 < n) { i += 2; continue; }
                m_rx_pending.push_back(buf[i]);
            }
        } else if (n == 0 || !socket_would_block()) {
            modem_hangup(true);
        }
    }

    /* Respect console RTS flow control when P3[1:0] output pair is enabled:
       RTS low (false) = ready to receive; RTS high (true) = hold off. */
    const bool rts_allows = !p3_dtr_configured || !gc_serial_rts(&machine);
    if (!m_rx_pending.empty() && rts_allows && cpu.uart_rx_idle()) {
        if (cpu.queue_uart_receive(m_rx_pending.front()))
            m_rx_pending.pop_front();
    }
}

} // namespace standalone
