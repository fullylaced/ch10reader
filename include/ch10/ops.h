// ch10/ops.h
// File operations that produce new Chapter 10 files:
//   Cut    - copy a time range and/or a subset of channels, data types and
//            1553 messages (optionally embedding annotations)
//   Split  - break a recording into parts by duration or size
//   Merge  - combine several recordings into one, time-ordered, with the RTC
//            of each rebased onto a common clock and channel IDs remapped
//
// Every output starts with the setup record (TMATS) and a time packet so it
// opens as a normal recording in any Chapter 10 tool.

#pragma once

#include <cstdint>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

#include "ch10/annotation.h"
#include "ch10/session.h"

namespace ch10 {

struct Ms1553Filter {
    bool          enabled = false;
    std::set<int> rts;              // keep messages to/from these RTs (empty = any)
    std::set<int> sas;              // keep these subaddresses (empty = any)
    int           tr = -1;          // -1 any, 0 receive, 1 transmit
    bool          errors_only = false;
};

struct FilterSpec {
    std::set<int>     channels;     // keep only these channel IDs (empty = all)
    std::set<uint8_t> drop_types;   // drop packets of these data types
    uint64_t          rtc_from = 0;
    uint64_t          rtc_to   = UINT64_MAX;
    Ms1553Filter      m1553;
    // Setup records and time packets are always kept so the output stays a
    // valid, time-correlated recording.
};

// A packet to insert while copying (annotations use this internally).
struct InjectPacket {
    uint64_t             rtc = 0;
    uint16_t             channel = 0;
    uint8_t              data_type = 0;
    uint8_t              flags = 0x02;        // 16-bit data checksum
    std::vector<uint8_t> body;
};

// A channel that injected packets create, described in the setup record.
struct InjectChannel {
    uint16_t    channel = 0;
    uint8_t     data_type = 0;
    std::string name;                         // R-x\DSI-n
};

struct Injection {
    std::vector<InjectPacket>  packets;
    std::vector<InjectChannel> channels;
    bool empty() const { return packets.empty(); }
};

struct OpResult {
    uint64_t packets_in  = 0;
    uint64_t packets_out = 0;
    uint64_t bytes_out   = 0;
    std::vector<std::filesystem::path> outputs;
    std::string warning;            // non-fatal notes (e.g. IPTS not rebased)
    std::string error;
};

// Copy `src` to `out` applying `spec`; `extra` annotations are written as
// CH10ANN1 packets at their time. Returns false on error (see result.error).
bool Cut(Session& src, const FilterSpec& spec, const std::vector<Annotation>& extra,
         const std::filesystem::path& out, OpResult& result,
         const ProgressFn& progress = nullptr);
// Same, also inserting `inject` packets (in time order) and describing their
// new channels in the TMATS setup record.
bool Cut(Session& src, const FilterSpec& spec, const std::vector<Annotation>& extra,
         const Injection& inject, const std::filesystem::path& out, OpResult& result,
         const ProgressFn& progress = nullptr);

// Build a 1553 F1 message (IPTS + IPDH + words) and a whole packet body.
std::vector<uint8_t> Build1553Message(uint64_t rtc, bool bus_b, int rt, bool transmit, int sa,
                                      const std::vector<uint16_t>& data);
std::vector<uint8_t> Build1553PacketBody(const std::vector<std::vector<uint8_t>>& messages);

// Split by duration (seconds > 0) or by size (max_bytes > 0). Outputs are
// named <prefix>_part01.ch10, _part02.ch10, ...
bool Split(Session& src, double seconds, uint64_t max_bytes,
           const std::filesystem::path& prefix, OpResult& result,
           const ProgressFn& progress = nullptr);

struct MergeInput {
    std::filesystem::path path;
    int channel_offset = 0;         // added to every channel ID except 0
};

// Merge recordings into one file. Time alignment uses each file's Time F1
// reference when all inputs have one; otherwise files are placed one after
// another with a one-second gap. Intra-packet time stamps are rebased for
// 1553 F1, Video F0/F1, Recording Event and annotation packets.
bool Merge(const std::vector<MergeInput>& inputs, const std::filesystem::path& out,
           OpResult& result, const ProgressFn& progress = nullptr);

// Helpers shared with front ends: build the per-channel offsets that avoid
// ID collisions (0, then the next multiple of 100 above the previous max).
std::vector<int> SuggestChannelOffsets(const std::vector<Session*>& sessions);

} // namespace ch10
