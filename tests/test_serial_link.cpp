#include "serial_link.hpp"

#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>

namespace standalone {
struct Sm8521CpuTestAccess {
    static uint8_t read(Sm8521Cpu& cpu, uint32_t address)
    {
        return cpu.mem_readbyte(address);
    }

    static void write(Sm8521Cpu& cpu, uint32_t address, uint8_t value)
    {
        cpu.mem_writebyte(address, value);
    }

    static void advance(Sm8521Cpu& cpu, uint64_t cycles)
    {
        cpu.m_cycles += cycles;
        cpu.advance_devices(static_cast<unsigned>(cycles));
    }
};
} // namespace standalone

namespace {

using Access = standalone::Sm8521CpuTestAccess;

struct ConsoleRig {
    std::unique_ptr<GcMachine, decltype(&std::free)> machine{
        static_cast<GcMachine *>(std::calloc(1, sizeof(GcMachine))), &std::free
    };
    std::unique_ptr<standalone::Sm8521Cpu> cpu;

    ConsoleRig()
    {
        assert(machine != nullptr);
        gc_machine_init(machine.get());
        cpu = std::make_unique<standalone::Sm8521Cpu>(*machine);
        /* Configure 9600-baud 8N1 (TM0C=0x80, TM0D=8 -> 5120 phi0 cycles/byte)
           and P3C=0x82 (P3[1:0]=RTS/DTR out, P3[3:2]=DSR/CTS in), asserting DTR/RTS low. */
        Access::write(*cpu, 0x50, 0x80);
        Access::write(*cpu, 0x51, 8);
        Access::write(*cpu, 0x2E, 0x1C);
        gc_bus_write(machine.get(), 0x23, 0x82);
        gc_bus_write(machine.get(), 0x17, 0x40); /* Slot 1 + RTS=0 + DTR=0 (ready). */
    }

    void send_str(const std::string& s, standalone::SerialLink& link)
    {
        for (char c : s) {
            while ((Access::read(*cpu, 0x2D) & 0x02u) == 0u) {
                Access::advance(*cpu, 256);
                link.poll(*machine, *cpu);
            }
            Access::write(*cpu, 0x2B, static_cast<uint8_t>(c));
            Access::advance(*cpu, 5120);
            link.poll(*machine, *cpu);
        }
    }

    std::string drain_rx(standalone::SerialLink& link, unsigned max_slices = 4000)
    {
        std::string out;
        for (unsigned i = 0; i < max_slices; ++i) {
            link.poll(*machine, *cpu);
            Access::advance(*cpu, 512);
            const uint8_t st = Access::read(*cpu, 0x2D);
            assert((st & 0x10u) == 0u); /* Flow-paced: must never trigger UART overrun! */
            if (st & 0x01u)
                out.push_back(static_cast<char>(Access::read(*cpu, 0x2C)));
        }
        return out;
    }
};

void test_peer_crossover()
{
    ConsoleRig a, b;
    standalone::SerialLink link;
    link.attach_peer(*b.machine, *b.cpu);

    /* Crossover maps A's DTR/RTS low (0) onto B's DSR/CTS low (0) on P3[3:2]. */
    link.poll(*a.machine, *a.cpu);
    assert((gc_bus_read(a.machine.get(), 0x17) & 0x0Cu) == 0x00u);
    assert((gc_bus_read(b.machine.get(), 0x17) & 0x0Cu) == 0x00u);

    Access::write(*a.cpu, 0x2B, 0x5Au);
    Access::write(*b.cpu, 0x2B, 0xA5u);
    Access::advance(*a.cpu, 5120);
    Access::advance(*b.cpu, 5120);
    link.poll(*a.machine, *a.cpu);
    Access::advance(*a.cpu, 5120);
    Access::advance(*b.cpu, 5120);

    assert((Access::read(*b.cpu, 0x2D) & 0x01u) != 0u);
    assert(Access::read(*b.cpu, 0x2C) == 0x5Au);
    assert((Access::read(*a.cpu, 0x2D) & 0x01u) != 0u);
    assert(Access::read(*a.cpu, 0x2C) == 0xA5u);
}

void test_tcp_link_cable()
{
    ConsoleRig host_rig, client_rig;
    standalone::SerialLink host_link, client_link;
    std::string err;
    assert(host_link.listen_tcp(0, &err)); /* Ephemeral port on 127.0.0.1 */
    const uint16_t port = host_link.bound_port();
    assert(port != 0u);
    assert(client_link.connect_tcp("127.0.0.1", port, &err));

    for (int i = 0; i < 50 && (!host_link.is_connected() || !client_link.is_connected()); ++i) {
        host_link.poll(*host_rig.machine, *host_rig.cpu);
        client_link.poll(*client_rig.machine, *client_rig.cpu);
    }
    assert(host_link.is_connected() && client_link.is_connected());

    host_rig.send_str("P1->P2", host_link);
    client_rig.send_str("P2->P1", client_link);

    std::string got_client, got_host;
    for (int i = 0; i < 600 && (got_client.size() < 6u || got_host.size() < 6u); ++i) {
        host_link.poll(*host_rig.machine, *host_rig.cpu);
        client_link.poll(*client_rig.machine, *client_rig.cpu);
        Access::advance(*host_rig.cpu, 512);
        Access::advance(*client_rig.cpu, 512);
        if (Access::read(*client_rig.cpu, 0x2D) & 0x01u)
            got_client.push_back(static_cast<char>(Access::read(*client_rig.cpu, 0x2C)));
        if (Access::read(*host_rig.cpu, 0x2D) & 0x01u)
            got_host.push_back(static_cast<char>(Access::read(*host_rig.cpu, 0x2C)));
    }
    assert(got_client == "P1->P2");
    assert(got_host == "P2->P1");
    assert((gc_bus_read(host_rig.machine.get(), 0x17) & 0x0Cu) == 0x00u);
    assert((gc_bus_read(client_rig.machine.get(), 0x17) & 0x0Cu) == 0x00u);
}

void test_virtual_hayes_modem()
{
    ConsoleRig rig;
    standalone::SerialLink modem;
    modem.enable_virtual_modem();
    modem.poll(*rig.machine, *rig.cpu);

    /* Modem asserts CTS=0 and DSR=0 on P3[3:2]. */
    assert((gc_bus_read(rig.machine.get(), 0x17) & 0x0Cu) == 0x00u);

    rig.send_str("ATE0V1\r", modem);
    const std::string ok_resp = rig.drain_rx(modem, 600);
    assert(ok_resp.find("OK") != std::string::npos);

    rig.send_str("ATDT5551212\r", modem);
    const std::string banner = rig.drain_rx(modem, 3000);
    assert(modem.modem_online());
    assert(banner.find("CONNECT 9600") != std::string::npos);
    assert(banner.find("TIGER GAME.COM PORTAL") != std::string::npos);

    rig.send_str("1\r", modem);
    const std::string status_page = rig.drain_rx(modem, 2500);
    assert(status_page.find("SM8521 @ 5.000 MHZ") != std::string::npos);

    rig.send_str("4\r", modem);
    const std::string bye = rig.drain_rx(modem, 1500);
    assert(!modem.modem_online());
    assert(bye.find("NO CARRIER") != std::string::npos);
}

} // namespace

int main()
{
    test_peer_crossover();
    test_tcp_link_cable();
    test_virtual_hayes_modem();
    std::cout << "SM8521 serial link tests passed (2P Compete.com crossover, TCP link cable, 9600 Hayes modem & portal).\n";
    return 0;
}
