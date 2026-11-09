// ch10/ethernet.h
// Ethernet Data Packets, Format 0 (10.6.15.1, raw IEEE 802.3 MAC frames) and
// Format 1 (10.6.15.2, ARINC-664 UDP messages), header parsing (MAC, VLAN,
// IPv4, UDP, TCP, ARP) and PCAP export for Wireshark.

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "ch10/session.h"

namespace ch10 {
namespace eth {

// One captured frame (F0) or message (F1). `data` points into the packet
// body that was decoded, so keep the Packet alive while using it.
struct Frame {
    uint64_t rtc = 0;            // intra-packet time (RTC form) or packet RTC
    uint16_t length = 0;
    uint8_t  netid = 0;
    uint8_t  speed = 0;          // F0: 0 auto, 1 10M, 2 100M, 3 1G, 4 10G
    bool     payload_only = false;   // F0 CONTENT=01: frame starts at EtherType payload
    bool     fce = false, fe = false, dce = false, le = false;   // error bits
    // F1 (ARINC-664) header fields
    bool     arinc664 = false;
    uint16_t virtual_link = 0;
    uint32_t src_ip = 0, dst_ip = 0;
    uint16_t src_port = 0, dst_port = 0;
    uint8_t  error = 0, flags = 0;
    const uint8_t* data = nullptr;

    std::string ErrorFlags() const;
};

struct Csdw { uint8_t format = 0; uint8_t ttb = 0; uint16_t frames = 0; uint16_t iph_len = 0; };

bool DecodeF0(const uint8_t* body, std::size_t len, uint8_t packet_flags, uint64_t packet_rtc,
              Csdw& csdw, std::vector<Frame>& out);
bool DecodeF1(const uint8_t* body, std::size_t len, uint8_t packet_flags, uint64_t packet_rtc,
              Csdw& csdw, std::vector<Frame>& out);
// Dispatch on data type (0x68 / 0x69).
bool Decode(uint8_t data_type, const uint8_t* body, std::size_t len, uint8_t packet_flags,
            uint64_t packet_rtc, Csdw& csdw, std::vector<Frame>& out);

// Parsed protocol headers of a raw MAC frame.
struct Parsed {
    bool     ok = false;
    uint8_t  dst_mac[6] = {0}, src_mac[6] = {0};
    uint16_t ethertype = 0;
    bool     vlan = false;
    uint16_t vlan_id = 0;
    bool     ipv4 = false, ipv6 = false, arp = false;
    uint32_t src_ip = 0, dst_ip = 0;
    uint8_t  ip_proto = 0;      // 6 TCP, 17 UDP, 1 ICMP
    bool     has_ports = false;
    uint16_t src_port = 0, dst_port = 0;
    const uint8_t* payload = nullptr;   // innermost payload we understood
    std::size_t    payload_len = 0;

    std::string Protocol() const;        // "UDP", "TCP", "ARP", "ICMP", "IPv6", "0x88B5"
    std::string Source() const;          // "192.168.1.5:5000" or MAC
    std::string Destination() const;
    std::string Info() const;            // short human summary
};
Parsed Parse(const uint8_t* frame, std::size_t len, bool payload_only = false);
// For F1 messages: fill Parsed from the IPDH fields (no MAC header present).
Parsed ParseArinc664(const Frame& f);

std::string MacString(const uint8_t* mac);
std::string IpString(uint32_t ip_host_order);

// Export every Ethernet frame of `channel` (-1 = all Ethernet channels) to
// a classic libpcap file (link type Ethernet). ARINC-664 messages get a
// synthesised MAC/IPv4/UDP header so their payload opens in Wireshark too.
// Timestamps are absolute when the recording has a time packet: day-of-year
// files count days from 1970-01-01 (year unknown), DMY files are exact.
bool WritePcap(Session& s, int channel, const std::filesystem::path& out,
               uint64_t& frames_written, const ProgressFn& progress = nullptr);

} // namespace eth
} // namespace ch10
