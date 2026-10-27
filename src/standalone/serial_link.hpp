#ifndef GAMECOM_STANDALONE_SERIAL_LINK_HPP
#define GAMECOM_STANDALONE_SERIAL_LINK_HPP

#include "gamecom.h"
#include "sm8521_cpu.hpp"

#include <cstdint>
#include <deque>
#include <map>
#include <string>

namespace standalone {

// External serial port subsystem for Tiger Game.com (SM8521 UART + P3[3:0]):
//   1. PeerCrossover: in-process 2-player Compete.com null-modem crossover
//   2. TcpHost / TcpClient: non-blocking TCP 2-player Compete.com link cable
//   3. VirtualModem: Hayes AT modem + MidiLink-style phonebook / TCP bridge +
//      built-in offline Tiger Game.com text portal for the Internet cartridge.
class SerialLink {
public:
    enum class Mode {
        Disconnected = 0,
        PeerCrossover,
        TcpHost,
        TcpClient,
        VirtualModem
    };

    SerialLink() = default;
    ~SerialLink();

    SerialLink(const SerialLink&) = delete;
    SerialLink& operator=(const SerialLink&) = delete;

    void disconnect();
    Mode mode() const noexcept { return m_mode; }
    bool is_connected() const noexcept;
    std::string status_summary() const;

    // In-process 2-player Compete.com crossover between two emulated consoles.
    void attach_peer(GcMachine& peer_machine, Sm8521Cpu& peer_cpu);
    static void step_crossover_pair(GcMachine& a_machine, Sm8521Cpu& a_cpu,
                                    GcMachine& b_machine, Sm8521Cpu& b_cpu);

    // TCP 2-player Compete.com link cable over localhost / LAN.
    bool listen_tcp(uint16_t port, std::string *error = nullptr);
    bool connect_tcp(const std::string& host, uint16_t port, std::string *error = nullptr);
    uint16_t bound_port() const noexcept { return m_bound_port; }

    // Virtual Hayes modem (9600 8N1 AT command set + TCP dialer + text portal).
    void enable_virtual_modem();
    bool load_phonebook(const std::string& path, std::string *error = nullptr);
    void set_phonebook_entry(const std::string& number, const std::string& target);
    bool modem_online() const noexcept { return m_modem_online; }

    // Call regularly from the emulation loop after stepping Sm8521Cpu.
    void poll(GcMachine& machine, Sm8521Cpu& cpu);

private:
    void close_sockets() noexcept;
    bool open_tcp_connection(const std::string& host, uint16_t port, std::string *error);
    void poll_tcp_link(GcMachine& machine, Sm8521Cpu& cpu);
    void poll_modem(GcMachine& machine, Sm8521Cpu& cpu);
    void modem_reset_defaults();
    void modem_queue_str(const std::string& s);
    void modem_result(int numeric_code, const char *verbose_text);
    void modem_hangup(bool emit_no_carrier);
    void modem_handle_command(const std::string& raw_cmd);
    void modem_dial(const std::string& dial_str);
    void portal_enter();
    void portal_handle_byte(uint8_t ch);

    Mode m_mode = Mode::Disconnected;

    // Peer crossover pointers (non-owning).
    GcMachine *m_peer_machine = nullptr;
    Sm8521Cpu *m_peer_cpu = nullptr;

    // Socket descriptors (-1 when closed).
    int m_listen_fd = -1;
    int m_conn_fd = -1;
    uint16_t m_bound_port = 0;
    std::string m_target_host;
    uint16_t m_target_port = 0;

    // Framed TCP link state ([0xA5, cmd, payload]).
    uint8_t m_rx_frame[3]{};
    unsigned m_rx_frame_len = 0;
    bool m_remote_dtr = true;
    bool m_remote_rts = true;
    bool m_last_sent_dtr = true;
    bool m_last_sent_rts = true;
    bool m_sent_initial_lines = false;
    std::deque<uint8_t> m_rx_pending;

    // Virtual Hayes modem state.
    bool m_modem_echo = true;
    bool m_modem_quiet = false;
    bool m_modem_verbose = true;
    bool m_modem_online = false;
    bool m_modem_portal_active = false;
    uint8_t m_modem_dtr_mode = 2;
    uint8_t m_modem_s0 = 0;
    bool m_prev_dtr_low = false;
    unsigned m_plus_count = 0;
    uint64_t m_last_tx_cycle = 0;
    std::string m_cmd_line;
    std::string m_portal_line;
    std::map<std::string, std::string> m_phonebook;
};

bool parse_host_port(const std::string& spec, std::string& out_host,
                     uint16_t& out_port, uint16_t default_port = 23);

} // namespace standalone

#endif
