// ch10/audio.h
// Audio from a Chapter 10 recording.
//
// Every source is a sampleable timeline: Render() asks for `frames` mono
// samples at times rtc, rtc+step, rtc+2*step, ... where `step` is in RTC
// ticks (10 MHz) and may be fractional or negative. A player renders at its
// output rate with step = speed * 1e7 / out_rate, so slow motion, fast
// forward and rewind fall out of the same call (pitch follows speed, like a
// tape machine).
//
// Sources:
//   AnalogAudioSource  - Analog Data Format 1 packets (10.6.5.2): the usual
//                        way cockpit/intercom audio is recorded
//   Ms1553AudioSource  - PCM samples carried in MIL-STD-1553 data words
//   TsAudioSource      - the audio stream muxed inside a Video F0/F1
//                        transport stream (needs FFmpeg)

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "ch10/session.h"

namespace ch10 {

class AudioSource {
public:
    virtual ~AudioSource() = default;
    const std::string& name() const { return name_; }
    double   sample_rate() const { return rate_; }      // native rate
    uint64_t first_rtc()   const { return first_rtc_; }
    uint64_t last_rtc()    const { return last_rtc_; }
    const std::string& error() const { return error_; }
    const std::string& info()  const { return info_; }  // codec / format summary

    // Fill `out` with `frames` mono samples in [-1, 1]. Returns the number of
    // frames that had data (others are written as silence).
    virtual int Render(double rtc, double step, int frames, float* out) = 0;

protected:
    std::string name_, error_, info_;
    double      rate_ = 0;
    uint64_t    first_rtc_ = 0, last_rtc_ = 0;
};

// ---------------------------------------------------------------------------
// Analog Format 1. Sample rate comes from TMATS (R-x\ASR-n-m) when present,
// otherwise it is measured from samples-per-packet and packet spacing.
struct AnalogInfo {
    bool same = true;
    int  factor = 0, totchan = 1, subchan = 0, bits = 16, mode = 1;
    int  samples_per_packet = 0;     // per subchannel
    const char* ModeName() const;
};
AnalogInfo DecodeAnalogCsdw(uint32_t csdw);

class AnalogAudioSource : public AudioSource {
public:
    // `subchannel_index` selects one of TOTCHAN interleaved subchannels.
    bool Open(const Session& s, int channel, int subchannel_index = 0,
              bool signed_samples = true, double rate_override = 0);
    int Render(double rtc, double step, int frames, float* out) override;
    const AnalogInfo& analog_info() const { return ainfo_; }
private:
    bool DecodePacket(uint32_t index);
    const Session* session_ = nullptr;
    std::unique_ptr<Reader> reader_;
    std::vector<uint32_t> pkts_;
    int  sub_ = 0;
    bool signed_ = true;
    AnalogInfo ainfo_;
    // decoded cache: packet index -> samples
    std::vector<std::pair<uint32_t, std::vector<float>>> cache_;
    std::size_t cache_hint_ = 0;
};

// ---------------------------------------------------------------------------
// PCM inside 1553 data words.
struct Ms1553AudioSpec {
    int channel = -1;
    int rt = -1;                 // -1 any
    int sa = -1;                 // -1 any
    int tr = -1;                 // -1 any, 0 receive, 1 transmit
    int first_word = 0;          // first data word carrying audio
    int word_count = -1;         // -1 = all remaining data words
    enum Format { S16, U16, S8x2, U8x2, MuLawx2, ALawx2 } format = S16;
    double sample_rate = 0;      // 0 = derive from message cadence
};

class Ms1553AudioSource : public AudioSource {
public:
    bool Open(const Session& s, const Ms1553AudioSpec& spec,
              const ProgressFn& progress = nullptr);
    int Render(double rtc, double step, int frames, float* out) override;
    uint64_t messages() const { return msgs_.size(); }
    uint64_t samples()  const { return samples_.size(); }
private:
    struct Msg { uint64_t rtc; uint32_t offset; uint32_t count; double period; };
    std::vector<Msg>     msgs_;
    std::vector<int16_t> samples_;
};

// ---------------------------------------------------------------------------
// Audio stream inside a video channel's transport stream (FFmpeg).
class TsAudioSource : public AudioSource {
public:
    TsAudioSource();
    ~TsAudioSource() override;
    bool Open(const Session& s, int channel);
    int Render(double rtc, double step, int frames, float* out) override;
    // Cheap check used to list candidate sources; fills codec/rate/channels.
    static bool Probe(const Session& s, int channel, std::string* codec,
                      int* rate, int* channels);
    static bool Available();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ---------------------------------------------------------------------------
// Render a source between two times at `rate` into a 16-bit mono WAV file.
bool WriteWav(AudioSource& src, uint64_t rtc_from, uint64_t rtc_to, int rate,
              const std::filesystem::path& out, const ProgressFn& progress = nullptr);

} // namespace ch10
