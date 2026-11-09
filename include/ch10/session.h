// ch10/session.h
// High-level "one open file" API shared by the CLI and the GUI.
//
// A Session scans a Chapter 10 file once (TMATS, first Time F1 packet, per-
// channel statistics) and then offers streaming access to 1553 messages and
// video payloads plus the export operations. Every long operation takes an
// optional ProgressFn so a front end can show progress and cancel.

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <map>

#include "ch10/annotation.h"
#include "ch10/ms1553_f1.h"
#include "ch10/reader.h"
#include "ch10/time_f1.h"
#include "ch10/tmats.h"
#include "ch10/video.h"

namespace ch10 {

// Called periodically with bytes processed / total. Return false to cancel.
using ProgressFn = std::function<bool(uint64_t done, uint64_t total)>;

struct ChannelStats {
    int         channel_id = 0;
    uint8_t     data_type  = 0;
    uint64_t    packets    = 0;
    uint64_t    bytes      = 0;
    uint64_t    messages   = 0;    // 1553 messages / video MPEG packets
    std::string source;            // TMATS R-x\DSI-n
    std::string tmats_type;        // TMATS R-x\CDT-n
    video::Info video;             // valid when data_type is Video F0/F1
};

// One entry per packet in the file, built during Open(). Enough to list,
// seek, and re-read any packet without rescanning the file.
struct PacketIndexEntry {
    uint64_t offset     = 0;   // file offset of the packet header
    uint64_t rtc        = 0;   // header RTC (10 MHz)
    uint32_t packet_len = 0;
    uint32_t data_len   = 0;
    uint16_t channel    = 0;
    uint8_t  data_type  = 0;
    uint8_t  seq        = 0;
    uint8_t  flags      = 0;
};

// Byte counts per time bin for one channel — the data-flow timeline.
struct ActivityRow {
    int                   channel_id = 0;
    uint8_t               data_type  = 0;
    std::vector<uint64_t> bytes;    // kActivityBins entries over [rtc_min, rtc_max]
    uint64_t              max_bytes = 0;
};
constexpr int kActivityBins = 1024;

// A fully formatted 1553 message, ready to display or write to CSV.
struct Message1553Row {
    uint64_t    index      = 0;
    int         channel    = 0;
    std::string time;              // correlated time, or "RTC x.xxxxxx s"
    uint64_t    rtc        = 0;
    char        bus        = 'A';
    std::string type;              // "BC->RT", "RT->RT", "Mode Code", ...
    uint16_t    cmd1       = 0;
    uint16_t    cmd2       = 0;
    bool        has_cmd2   = false;
    std::string cmd1_text;         // "RT05 R SA01 WC04"
    std::string cmd2_text;
    int         rt = 0, sa = 0, wc_mc = 0;
    char        tr         = 'R';
    uint16_t    status1 = 0, status2 = 0;
    bool        has_status1 = false, has_status2 = false;
    std::string status1_flags, status2_flags;
    std::string errors;            // "ME|TO" etc.
    int         gap1 = 0, gap2 = 0;
    std::vector<uint16_t> data;    // data words (or all words when errored)
    std::string data_hex;          // "1111 2222 ..."
};

class Session {
public:
    // Scan the file. Returns false on failure (see error()).
    bool Open(const std::filesystem::path& path,
              const ProgressFn& progress = nullptr);
    bool IsOpen() const { return open_; }
    void Close();

    const std::filesystem::path& path() const { return path_; }
    const std::string& error() const { return error_; }

    uint64_t file_size()    const { return file_size_; }
    uint64_t packet_count() const { return packet_count_; }
    uint64_t resync_count() const { return resync_count_; }

    const Tmats&                     tmats()    const { return tmats_; }
    const TimeCorrelator&            time()     const { return time_; }
    const std::vector<ChannelStats>& channels() const { return channels_; }
    std::vector<int> ChannelsOfType(uint8_t data_type) const;
    const ChannelStats* Channel(int id) const;
    bool IsVideoType(uint8_t t) const { return t == 0x40 || t == 0x41; }

    // ---- Packet index / time range -----------------------------------------
    const std::vector<PacketIndexEntry>& packets() const { return index_; }
    uint64_t rtc_min() const { return rtc_min_; }   // earliest packet RTC
    uint64_t rtc_max() const { return rtc_max_; }   // latest packet RTC
    double   duration_s() const { return (rtc_max_ - rtc_min_) / 1e7; }

    // Absolute time (via the first Time F1 packet) or "RTC x s" fallback.
    std::string TimeString(uint64_t rtc) const { return time_.RtcToString(rtc); }
    // "+HH:MM:SS.mmm" from the start of the recording.
    std::string RelativeString(uint64_t rtc) const;
    std::string RecordingStart() const { return TimeString(rtc_min_); }
    std::string RecordingEnd()   const { return TimeString(rtc_max_); }
    uint64_t    time_packet_count() const { return time_packets_; }

    // Index (into packets()) of the first packet with rtc >= `rtc`, in time
    // order; packets().size() if none. Optionally restricted to one channel.
    std::size_t FindByTime(uint64_t rtc, int channel = -1) const;
    // Packet indices for one channel, in file order.
    const std::vector<uint32_t>& ChannelPackets(int channel) const;
    // Indices of all packets sorted by RTC.
    const std::vector<uint32_t>& ByTime() const { return by_time_; }

    // Read any packet by index (random access on a private file handle).
    bool ReadPacket(std::size_t index, Packet& out);

    // Data-flow timeline rows, one per channel, in channel order.
    const std::vector<ActivityRow>& activity() const { return activity_; }

    // ---- Annotations and anomalies -----------------------------------------
    // Annotations read from the file (CH10ANN1 packets and Recording Events)
    // plus any added in this session (source == User).
    const std::vector<Annotation>& annotations() const { return annotations_; }
    void AddAnnotation(const Annotation& a);
    bool RemoveAnnotation(std::size_t index);   // only User annotations can go
    std::vector<Annotation> UserAnnotations() const;
    // Problems noticed while scanning (capped; anomaly_total() has the count).
    const std::vector<Annotation>& anomalies() const { return anomalies_; }
    uint64_t anomaly_total() const { return anomaly_total_; }

    // ---- TMATS ------------------------------------------------------------
    bool ExportTmats(const std::filesystem::path& out) const;

    // ---- MIL-STD-1553 -----------------------------------------------------
    // Stream messages from `channel` (-1 = all 1553 channels), at most
    // `max_msgs` (-1 = unlimited). `fn` returns false to stop.
    bool ForEach1553(int channel, long max_msgs, bool correlate_time,
                     const std::function<bool(const Message1553Row&)>& fn,
                     const ProgressFn& progress = nullptr);

    // Convenience: collect rows into a vector.
    bool Load1553(int channel, long max_msgs, bool correlate_time,
                  std::vector<Message1553Row>& out,
                  const ProgressFn& progress = nullptr);

    // Write the full CSV (re-reads the file; memory-flat for huge recordings).
    bool Export1553Csv(const std::filesystem::path& out, int channel,
                       bool correlate_time,
                       const ProgressFn& progress = nullptr);

    static std::string CsvHeader();
    static std::string CsvLine(const Message1553Row& r);

    // ---- Video ------------------------------------------------------------
    struct VideoResult {
        uint64_t bytes   = 0;
        uint64_t packets = 0;
        bool     raw_copy_warning = false;   // F1 program stream with IPH
    };
    bool ExtractVideo(int channel, const std::filesystem::path& out,
                      VideoResult& result,
                      const ProgressFn& progress = nullptr);

private:
    bool Fail(const std::string& msg) { error_ = msg; return false; }

    bool                     open_         = false;
    std::filesystem::path    path_;
    std::string              error_;
    uint64_t                 file_size_    = 0;
    uint64_t                 packet_count_ = 0;
    uint64_t                 resync_count_ = 0;
    Tmats                    tmats_;
    TimeCorrelator           time_;
    std::vector<ChannelStats> channels_;

    std::vector<PacketIndexEntry>            index_;
    std::vector<uint32_t>                    by_time_;
    std::map<int, std::vector<uint32_t>>     by_channel_;
    std::vector<ActivityRow>                 activity_;
    uint64_t                                 rtc_min_ = 0, rtc_max_ = 0;
    uint64_t                                 time_packets_ = 0;
    std::unique_ptr<class Reader>            reader_;   // random access
    std::vector<Annotation>                  annotations_;
    std::vector<Annotation>                  anomalies_;
    uint64_t                                 anomaly_total_ = 0;

    void NoteAnomaly(uint64_t rtc, int channel, uint64_t packet_index,
                     const std::string& text);
    void ScanEventPacket(const Packet& p, uint64_t index);
};

// Build a display row from a decoded message.
Message1553Row MakeRow(uint64_t index, int channel, uint8_t packet_flags,
                       const ms1553::Message& m, const TimeCorrelator& time,
                       bool correlate_time);

} // namespace ch10
