// ch10/packet.h
// IRIG 106 Chapter 10 common packet elements (IRIG 106-15, Section 10.6.1).
//
// All multi-byte fields in a Chapter 10 file are little-endian. To stay
// portable across compilers (MSVC/GCC/Clang) and architectures we never cast
// raw buffers to packed structs; we decode explicitly with the helpers below.

#pragma once

#include <cstdint>
#include <cstddef>
#include <string>

namespace ch10 {

// ---------------------------------------------------------------------------
// Little-endian byte decoding helpers (portable, alignment-safe)
// ---------------------------------------------------------------------------
inline uint16_t le16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}
inline uint32_t le32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}
inline uint64_t le48(const uint8_t* p) {           // 48-bit RTC field
    uint64_t v = 0;
    for (int i = 5; i >= 0; --i) v = (v << 8) | p[i];
    return v;
}
inline uint64_t le64(const uint8_t* p) {
    return static_cast<uint64_t>(le32(p)) |
           (static_cast<uint64_t>(le32(p + 4)) << 32);
}

// ---------------------------------------------------------------------------
// Constants (10.6.1.1)
// ---------------------------------------------------------------------------
constexpr uint16_t    kSyncPattern         = 0xEB25;
constexpr std::size_t kHeaderSize          = 24;
constexpr std::size_t kSecondaryHeaderSize = 12;
// Max packet size 2^19; setup records may reach 2^27 (Table 10-9).
constexpr uint32_t    kMaxPacketSize       = 0x08000000;

// Data types from Table 10-10 (the ones this tool cares about first).
enum DataType : uint8_t {
    kCompGenF0_UserDefined = 0x00,
    kCompGenF1_Tmats       = 0x01,   // Setup record (TMATS)
    kCompGenF2_Events      = 0x02,
    kCompGenF3_Index       = 0x03,
    kPcmF1                 = 0x09,
    kTimeF1                = 0x11,   // IRIG/GPS/RTC time
    kMilStd1553F1          = 0x19,   // MIL-STD-1553B bus data
    kMilStd1553F2_16PP194  = 0x1A,
    kAnalogF1              = 0x21,
    kDiscreteF1            = 0x29,
    kMessageF0             = 0x30,
    kArinc429F0            = 0x38,
    kVideoF0               = 0x40,   // MPEG-2/H.264 transport stream
    kVideoF1               = 0x41,
    kVideoF2               = 0x42,
    kEthernetF0            = 0x68,
};

const char* DataTypeName(uint8_t type);

// ---------------------------------------------------------------------------
// Packet header (10.6.1.1) — fixed 24 bytes
// ---------------------------------------------------------------------------
struct PacketHeader {
    uint16_t sync           = 0;   // must be 0xEB25
    uint16_t channel_id     = 0;
    uint32_t packet_len     = 0;   // whole packet incl. header + trailer
    uint32_t data_len       = 0;   // CSDW + IPTS + IPDH + data (no filler/cksum)
    uint8_t  data_type_ver  = 0;
    uint8_t  seq_num        = 0;
    uint8_t  flags          = 0;
    uint8_t  data_type      = 0;
    uint64_t rtc            = 0;   // 48-bit 10 MHz relative time counter
    uint16_t checksum       = 0;

    // Packet flag decodes (10.6.1.1.g)
    bool     HasSecondaryHeader()   const { return (flags & 0x80) != 0; }
    bool     IptsUsesSecondaryTime()const { return (flags & 0x40) != 0; }
    bool     RtcSyncError()         const { return (flags & 0x20) != 0; }
    bool     DataOverflow()         const { return (flags & 0x10) != 0; }
    uint8_t  SecondaryTimeFormat()  const { return (flags >> 2) & 0x03; }
    uint8_t  ChecksumType()         const { return flags & 0x03; } // 0 none,1=8,2=16,3=32 bit

    std::size_t BodyOffset() const {       // offset of CSDW from packet start
        return kHeaderSize + (HasSecondaryHeader() ? kSecondaryHeaderSize : 0);
    }
};

// Secondary header time formats (packet flags bits 3-2)
enum SecondaryTimeFmt : uint8_t {
    kSecTimeCh4BinaryWeighted = 0,  // Chapter 4 48-bit BCD-weighted
    kSecTimeIeee1588          = 1,  // seconds (MSLW) + nanoseconds (LSLW)
    kSecTimeErtc              = 2,  // 64-bit 1 ns extended relative counter
};

// Parse 24 header bytes. Returns false if the sync pattern is wrong.
bool ParseHeader(const uint8_t* buf, PacketHeader& out);

// 16-bit arithmetic sum of the first 11 little-endian 16-bit words
// (10.6.1.1.j). Compare against PacketHeader::checksum.
uint16_t HeaderChecksum(const uint8_t* buf24);

// Quick validity check used by the reader's resync scan.
bool HeaderLooksValid(const uint8_t* buf24);

// Data checksum over the packet body + filler (10.6.1.4). `body` points just
// past the header/secondary header, `len` = packet_len - headers - checksum.
uint32_t DataChecksum(const uint8_t* body, std::size_t len, uint8_t type);

} // namespace ch10
