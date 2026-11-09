// src/main.cpp
// ch10dump — IRIG 106 Chapter 10 command-line reader (thin front end over
// ch10::Session; the wxWidgets GUI in gui/ uses the same library).
//
//   ch10dump <file>                     channel/packet summary
//   ch10dump <file> --tmats [out.txt]   print or export the TMATS setup record
//   ch10dump <file> --m1553 [out.csv]   decode 1553 F1 messages (stdout or CSV)
//   ch10dump <file> --video <prefix>    extract video channels to <prefix>_chN.ts/.mpg
//   options: --channel <id>  --max <n>  --no-time

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

#include <set>
#include <sstream>

#include "ch10/audio.h"
#include "ch10/ethernet.h"
#include "ch10/message_f0.h"
#include "ch10/ops.h"
#include "ch10/userdata.h"
#include "ch10/wordmap.h"
#include "ch10/session.h"
#include "ch10/video_stream.h"

using namespace ch10;

namespace {

struct Options {
    std::string file;
    bool        do_tmats = false;
    std::string tmats_out;
    bool        do_1553 = false;
    std::string csv_out;
    bool        do_video = false;
    std::string video_prefix;
    bool        do_snapshot = false;
    double      snapshot_s  = 0;
    std::string snapshot_out;
    bool        do_annotations = false;
    bool        do_ethernet = false;
    std::string pcap_out;
    bool        do_messages = false;
    std::string inject_file;
    std::string wordmap_file;
    std::string template_out;
    std::string wav_out;
    int         wav_rate = 0;             // 0 = source rate
    int         subchannel = 0;
    std::string audio_fmt = "s16";
    int         first_word = 0, word_count = -1;
    // cut / split / merge
    std::string cut_out;
    double      from_s = -1, to_s = -1;
    std::set<int> keep_channels;
    bool        drop_video = false;
    ch10::Ms1553Filter m1553;
    std::vector<ch10::Annotation> annotate;
    std::string split_prefix;
    double      split_every = 0;
    double      split_mb = 0;
    std::string merge_out;
    std::vector<std::string> merge_files;
    int         channel_offset = -1;      // -1 = automatic
    int         channel  = -1;
    long        max_msgs = -1;
    bool        use_time = true;
};

void Usage() {
    std::puts(
        "ch10dump - IRIG 106 Chapter 10 reader (TMATS, Time F1, MIL-STD-1553 F1, Video F0/F1)\n"
        "\n"
        "usage: ch10dump <file.ch10> [options]\n"
        "  (no options)          per-channel packet summary\n"
        "  --tmats [out.txt]     print TMATS setup record (or write to file)\n"
        "  --m1553 [out.csv]     dump 1553 messages; with a filename, write CSV\n"
        "  --video <prefix>      extract video channels to <prefix>_ch<N>.ts/.mpg\n"
        "  --snapshot <sec> <out.ppm>  decode the video frame <sec> seconds into the\n"
        "                        recording (use --channel to pick the channel)\n"
        "  --annotations         list annotations, events and detected anomalies\n"
        "  --ethernet            list Ethernet frames (--channel, --max)\n"
        "  --pcap <out.pcap>     export Ethernet frames for Wireshark (--channel)\n"
        "  --messages            list Message Data (F0) packets as text\n"
        "  --wordmap <map.csv>   decode 1553 data words by name in --m1553 output\n"
        "  --wordmap-template <out.csv>   write an example word map\n"
        "  --inject <data.txt>   with --cut: add notes / text / 1553 messages from a\n"
        "                        user data file (see --inject-template)\n"
        "  --inject-template <out.txt>    write an annotated example user data file\n"
        "  --wav <out.wav>       export audio: --channel <analog or video channel>, or a 1553\n"
        "                        channel with --rt/--sa/--tr [--words first,count]\n"
        "                        [--fmt s16|u16|s8x2|u8x2|ulaw|alaw] [--rate <Hz>] [--from/--to]\n"
        "\n"
        "  --cut <out.ch10>      write a filtered copy; combine with:\n"
        "      --from <sec> --to <sec>     time range (seconds from start)\n"
        "      --channels 16,32            keep only these channels\n"
        "      --drop-video                drop video packets\n"
        "      --rt 5,7  --sa 1  --tr R|T  --errors-only   1553 message filter\n"
        "      --annotate \"text@sec\"  or  \"text@sec-sec\"  (repeatable) embed a note\n"
        "      --severity note|warning|anomaly|marker  (for the following --annotate)\n"
        "  --split <prefix> --every <sec> | --max-mb <mb>   split into parts\n"
        "  --merge <out.ch10> <file2> [<file3>...] [--channel-offset N|auto]\n"
        "  --channel <id>        only this channel ID\n"
        "  --max <n>             stop after n 1553 messages\n"
        "  --no-time             show raw RTC instead of correlated time\n");
}

bool NextArgIsValue(int argc, char** argv, int i) {
    return i + 1 < argc && argv[i + 1][0] != '-';
}

std::set<int> ParseIntList(const std::string& s) {
    std::set<int> out;
    std::stringstream ss(s);
    std::string tok;
    while (std::getline(ss, tok, ',')) if (!tok.empty()) out.insert(std::atoi(tok.c_str()));
    return out;
}

// "text@1.5" or "text@1.5-3.0" -> annotation with relative times (converted
// to RTC once the file is open; rtc fields hold seconds*1e7 meanwhile).
bool ParseAnnotate(const std::string& arg, ch10::Severity sev, ch10::Annotation& a) {
    std::size_t at = arg.rfind('@');
    if (at == std::string::npos) return false;
    a.text = arg.substr(0, at);
    std::string t = arg.substr(at + 1);
    std::size_t dash = t.find('-');
    double s0 = std::atof(t.c_str());
    double s1 = dash == std::string::npos ? s0 : std::atof(t.c_str() + dash + 1);
    a.rtc_start = static_cast<uint64_t>(s0 * 1e7);
    a.rtc_end   = static_cast<uint64_t>(std::max(s0, s1) * 1e7);
    a.severity = sev;
    return true;
}

bool ParseArgs(int argc, char** argv, Options& o) {
    if (argc < 2) return false;
    o.file = argv[1];
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--tmats") {
            o.do_tmats = true;
            if (NextArgIsValue(argc, argv, i)) o.tmats_out = argv[++i];
        } else if (a == "--m1553" || a == "--1553") {
            o.do_1553 = true;
            if (NextArgIsValue(argc, argv, i)) o.csv_out = argv[++i];
        } else if (a == "--video") {
            if (!NextArgIsValue(argc, argv, i)) return false;
            o.do_video = true;
            o.video_prefix = argv[++i];
        } else if (a == "--snapshot") {
            if (i + 2 >= argc) return false;
            o.do_snapshot  = true;
            o.snapshot_s   = std::atof(argv[++i]);
            o.snapshot_out = argv[++i];
        } else if (a == "--annotations") {
            o.do_annotations = true;
        } else if (a == "--ethernet") {
            o.do_ethernet = true;
        } else if (a == "--pcap") {
            if (!NextArgIsValue(argc, argv, i)) return false;
            o.pcap_out = argv[++i];
        } else if (a == "--messages") {
            o.do_messages = true;
        } else if (a == "--inject") {
            if (!NextArgIsValue(argc, argv, i)) return false;
            o.inject_file = argv[++i];
        } else if (a == "--wordmap") {
            if (!NextArgIsValue(argc, argv, i)) return false;
            o.wordmap_file = argv[++i];
        } else if (a == "--inject-template" || a == "--wordmap-template") {
            if (!NextArgIsValue(argc, argv, i)) return false;
            o.template_out = std::string(a == "--inject-template" ? "i:" : "w:") + argv[++i];
        } else if (a == "--wav") {
            if (!NextArgIsValue(argc, argv, i)) return false;
            o.wav_out = argv[++i];
        } else if (a == "--rate") {
            if (i + 1 >= argc) return false;
            o.wav_rate = std::atoi(argv[++i]);
        } else if (a == "--subchannel") {
            if (i + 1 >= argc) return false;
            o.subchannel = std::atoi(argv[++i]);
        } else if (a == "--fmt") {
            if (!NextArgIsValue(argc, argv, i)) return false;
            o.audio_fmt = argv[++i];
        } else if (a == "--words") {
            if (!NextArgIsValue(argc, argv, i)) return false;
            std::set<int> w = ParseIntList(argv[++i]);
            if (!w.empty()) { o.first_word = *w.begin(); if (w.size() > 1) o.word_count = *std::next(w.begin()); }
        } else if (a == "--cut") {
            if (!NextArgIsValue(argc, argv, i)) return false;
            o.cut_out = argv[++i];
        } else if (a == "--from") {
            if (i + 1 >= argc) return false;
            o.from_s = std::atof(argv[++i]);
        } else if (a == "--to") {
            if (i + 1 >= argc) return false;
            o.to_s = std::atof(argv[++i]);
        } else if (a == "--channels") {
            if (!NextArgIsValue(argc, argv, i)) return false;
            o.keep_channels = ParseIntList(argv[++i]);
        } else if (a == "--drop-video") {
            o.drop_video = true;
        } else if (a == "--rt") {
            if (!NextArgIsValue(argc, argv, i)) return false;
            o.m1553.rts = ParseIntList(argv[++i]); o.m1553.enabled = true;
        } else if (a == "--sa") {
            if (!NextArgIsValue(argc, argv, i)) return false;
            o.m1553.sas = ParseIntList(argv[++i]); o.m1553.enabled = true;
        } else if (a == "--tr") {
            if (!NextArgIsValue(argc, argv, i)) return false;
            o.m1553.tr = (argv[++i][0] == 'T' || argv[i][0] == 't') ? 1 : 0; o.m1553.enabled = true;
        } else if (a == "--errors-only") {
            o.m1553.errors_only = true; o.m1553.enabled = true;
        } else if (a == "--severity") {
            if (!NextArgIsValue(argc, argv, i)) return false;
            std::string v = argv[++i];
            o.annotate.push_back(ch10::Annotation());   // placeholder carries severity
            o.annotate.back().text = "\x01sev";
            o.annotate.back().severity = v == "warning" ? ch10::Severity::Warning :
                                         v == "anomaly" ? ch10::Severity::Anomaly :
                                         v == "marker"  ? ch10::Severity::Marker : ch10::Severity::Note;
        } else if (a == "--annotate") {
            if (i + 1 >= argc) return false;
            ch10::Severity sev = ch10::Severity::Note;
            if (!o.annotate.empty() && o.annotate.back().text == "\x01sev") {
                sev = o.annotate.back().severity; o.annotate.pop_back();
            }
            ch10::Annotation an;
            if (!ParseAnnotate(argv[++i], sev, an)) return false;
            o.annotate.push_back(an);
        } else if (a == "--split") {
            if (!NextArgIsValue(argc, argv, i)) return false;
            o.split_prefix = argv[++i];
        } else if (a == "--every") {
            if (i + 1 >= argc) return false;
            o.split_every = std::atof(argv[++i]);
        } else if (a == "--max-mb") {
            if (i + 1 >= argc) return false;
            o.split_mb = std::atof(argv[++i]);
        } else if (a == "--merge") {
            if (!NextArgIsValue(argc, argv, i)) return false;
            o.merge_out = argv[++i];
            while (NextArgIsValue(argc, argv, i)) o.merge_files.push_back(argv[++i]);
        } else if (a == "--channel-offset") {
            if (i + 1 >= argc) return false;
            std::string v = argv[++i];
            o.channel_offset = v == "auto" ? -1 : std::atoi(v.c_str());
        } else if (a == "--channel") {
            if (!NextArgIsValue(argc, argv, i)) return false;
            o.channel = std::atoi(argv[++i]);
        } else if (a == "--max") {
            if (!NextArgIsValue(argc, argv, i)) return false;
            o.max_msgs = std::atol(argv[++i]);
        } else if (a == "--no-time") {
            o.use_time = false;
        } else {
            std::fprintf(stderr, "unknown option: %s\n", a.c_str());
            return false;
        }
    }
    return true;
}

void PrintSummary(const Session& s) {
    std::printf("File: %s (%llu bytes, %llu packets",
                s.path().string().c_str(),
                static_cast<unsigned long long>(s.file_size()),
                static_cast<unsigned long long>(s.packet_count()));
    if (s.resync_count())
        std::printf(", %llu resyncs",
                    static_cast<unsigned long long>(s.resync_count()));
    std::puts(")");

    if (!s.tmats().empty()) {
        std::printf("TMATS: %s, %s format, %zu attributes",
                    s.tmats().Ch10VersionName(),
                    s.tmats().xml_format() ? "XML" : "ASCII",
                    s.tmats().attributes().size());
        std::string prog = s.tmats().Get("G\\PN");
        if (!prog.empty()) std::printf("  (G\\PN: %s)", prog.c_str());
        std::puts("");
    }
    if (s.time().HasReference())
        std::printf("Time reference: %s (%s, %s)\n",
                    s.time().reference().ToString().c_str(),
                    s.time().reference().FmtName(),
                    s.time().reference().SrcName());
    std::printf("Recording: %s -> %s  (%.3f s)\n",
                s.RecordingStart().c_str(), s.RecordingEnd().c_str(),
                s.duration_s());
    if (!s.annotations().empty() || s.anomaly_total())
        std::printf("Annotations: %zu   Anomalies detected: %llu   (--annotations to list)\n",
                    s.annotations().size(), static_cast<unsigned long long>(s.anomaly_total()));

    std::puts("\n  Chan  Packets      Bytes  Type                                        Source (TMATS)");
    std::puts("  ----  -------  ---------  ------------------------------------------  --------------");
    for (const auto& c : s.channels()) {
        std::printf("  %4d  %7llu  %9llu  0x%02X %-38s  %s",
                    c.channel_id,
                    static_cast<unsigned long long>(c.packets),
                    static_cast<unsigned long long>(c.bytes),
                    c.data_type, DataTypeName(c.data_type), c.source.c_str());
        if (c.messages)
            std::printf("  [%llu %s]",
                        static_cast<unsigned long long>(c.messages),
                        c.data_type == kMilStd1553F1 ? "msgs" : c.data_type == kEthernetF0 || c.data_type == 0x69 ? "frames" : c.data_type == kMessageF0 ? "msgs" : "TS pkts");
        std::puts("");
    }
}

int DumpTmats(const Options& o, const Session& s) {
    if (s.tmats().empty()) {
        std::fprintf(stderr, "no TMATS setup record found\n");
        return 1;
    }
    if (o.tmats_out.empty()) {
        std::fwrite(s.tmats().text().data(), 1, s.tmats().text().size(), stdout);
        std::puts("");
        return 0;
    }
    if (!s.ExportTmats(o.tmats_out)) {
        std::fprintf(stderr, "cannot write %s\n", o.tmats_out.c_str());
        return 1;
    }
    std::printf("TMATS written to %s (%zu bytes, %zu attributes)\n",
                o.tmats_out.c_str(), s.tmats().text().size(),
                s.tmats().attributes().size());
    return 0;
}

int Dump1553(const Options& o, Session& s) {
    ch10::WordMap wm;
    if (!o.wordmap_file.empty()) {
        std::string err;
        if (!wm.LoadCsv(o.wordmap_file, &err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 1; }
    }
    if (!o.csv_out.empty()) {
        if (!s.Export1553Csv(o.csv_out, o.channel, o.use_time)) {
            std::fprintf(stderr, "%s\n", s.error().c_str());
            return 1;
        }
        std::printf("CSV written to %s\n", o.csv_out.c_str());
        return 0;
    }
    long count = 0;
    s.ForEach1553(o.channel, o.max_msgs, o.use_time,
        [&count, &wm](const Message1553Row& r) {
            ++count;
            std::printf("#%-6llu ch%-3d %s  bus %c  %-13s %s",
                        static_cast<unsigned long long>(r.index), r.channel,
                        r.time.c_str(), r.bus, r.type.c_str(),
                        r.cmd1_text.c_str());
            if (r.has_cmd2) std::printf("  <- %s", r.cmd2_text.c_str());
            if (!r.errors.empty()) std::printf("  ERR[%s]", r.errors.c_str());
            std::puts("");
            if (r.has_status1) {
                std::printf("        status1 0x%04X", r.status1);
                if (!r.status1_flags.empty())
                    std::printf(" [%s]", r.status1_flags.c_str());
                std::puts("");
            }
            if (r.has_status2) {
                std::printf("        status2 0x%04X", r.status2);
                if (!r.status2_flags.empty())
                    std::printf(" [%s]", r.status2_flags.c_str());
                std::puts("");
            }
            if (!r.data_hex.empty())
                std::printf("        data    %s\n", r.data_hex.c_str());
            if (!r.data.empty()) {
                std::string ascii = ch10::WordsToAscii(r.data);
                bool readable = std::count_if(ascii.begin(), ascii.end(), [](char c) { return c != '.'; }) * 10 >= static_cast<long>(ascii.size()) * 7;
                if (readable) std::printf("        ascii   \"%s\"\n", ascii.c_str());
                for (const auto& dw : wm.Decode(r.channel, r.rt, r.sa, r.tr == 'T', r.data))
                    std::printf("        %-16s = %s %s\n", dw.name.c_str(), dw.value.c_str(), dw.units.c_str());
            }
            return true;
        });
    std::printf("-- %ld messages --\n", count);
    return 0;
}

int ExtractVideo(const Options& o, Session& s) {
    std::vector<int> ids;
    for (const auto& c : s.channels())
        if ((c.data_type == kVideoF0 || c.data_type == kVideoF1) &&
            (o.channel < 0 || c.channel_id == o.channel))
            ids.push_back(c.channel_id);
    if (ids.empty()) {
        std::fprintf(stderr, "no video packets found\n");
        return 1;
    }
    for (int id : ids) {
        const ChannelStats* c = s.Channel(id);
        std::string name = o.video_prefix + "_ch" + std::to_string(id) +
                           video::Extension(c->video);
        Session::VideoResult res;
        if (!s.ExtractVideo(id, name, res)) {
            std::fprintf(stderr, "%s\n", s.error().c_str());
            return 1;
        }
        std::printf("channel %d (%s): %llu bytes, %llu MPEG packets -> %s%s\n",
                    id, c->video.FormatName(),
                    static_cast<unsigned long long>(res.bytes),
                    static_cast<unsigned long long>(res.packets),
                    name.c_str(),
                    res.raw_copy_warning
                        ? "  [warning: PS with IPH copied raw]" : "");
    }
    return 0;
}

int Snapshot(const Options& o, Session& s) {
    int channel = o.channel;
    if (channel < 0) {
        for (const auto& c : s.channels())
            if (s.IsVideoType(c.data_type)) { channel = c.channel_id; break; }
    }
    if (channel < 0) { std::fprintf(stderr, "no video channel\n"); return 1; }

    VideoStream vs;
    if (!vs.Open(s, channel)) {
        std::fprintf(stderr, "%s\n", vs.error().c_str());
        return 1;
    }
    uint64_t rtc = s.rtc_min() + static_cast<uint64_t>(o.snapshot_s * 1e7);
    auto f = vs.SeekTo(rtc);
    if (!f) {
        std::fprintf(stderr, "no frame decoded: %s\n", vs.error().c_str());
        return 1;
    }
    std::FILE* fp = std::fopen(o.snapshot_out.c_str(), "wb");
    if (!fp) { std::fprintf(stderr, "cannot write %s\n", o.snapshot_out.c_str()); return 1; }
    std::fprintf(fp, "P6\n%d %d\n255\n", f->width, f->height);
    std::fwrite(f->rgb.data(), 1, f->rgb.size(), fp);
    std::fclose(fp);
    std::printf("channel %d (%s) %dx%d: frame at %s (requested %s) -> %s\n",
                channel, vs.codec_name().c_str(), f->width, f->height,
                s.TimeString(f->rtc).c_str(), s.TimeString(rtc).c_str(),
                o.snapshot_out.c_str());
    return 0;
}

int ListEthernet(const Options& o, Session& s) {
    ch10::Reader rd(s.path());
    ch10::Packet p;
    std::vector<ch10::eth::Frame> frames;
    ch10::eth::Csdw csdw;
    long count = 0;
    for (uint32_t i : s.ByTime()) {
        const auto& e = s.packets()[i];
        if (e.data_type != ch10::kEthernetF0 && e.data_type != 0x69) continue;
        if (o.channel >= 0 && e.channel != o.channel) continue;
        if (!rd.ReadAt(static_cast<std::streamoff>(e.offset), p)) continue;
        if (!ch10::eth::Decode(e.data_type, p.Body(), p.BodyLen(), p.header.flags, p.header.rtc, csdw, frames)) continue;
        for (const auto& f : frames) {
            if (o.max_msgs >= 0 && count >= o.max_msgs) return 0;
            ++count;
            ch10::eth::Parsed pa = f.arinc664 ? ch10::eth::ParseArinc664(f) : ch10::eth::Parse(f.data, f.length, f.payload_only);
            std::printf("#%-5ld ch%-3d %s  net%d  %4u B  %s%s%s\n", count, e.channel,
                        s.TimeString(f.rtc).c_str(), f.netid, f.length, pa.Info().c_str(),
                        f.ErrorFlags().empty() ? "" : "  ERR[", f.ErrorFlags().empty() ? "" : (f.ErrorFlags() + "]").c_str());
            if (pa.payload_len) {
                std::string txt;
                for (std::size_t k = 0; k < std::min<std::size_t>(pa.payload_len, 64); ++k) {
                    char c = static_cast<char>(pa.payload[k]); txt += (c >= 32 && c < 127) ? c : '.';
                }
                std::printf("        payload: %s%s\n", txt.c_str(), pa.payload_len > 64 ? "..." : "");
            }
        }
    }
    std::printf("-- %ld frames --\n", count);
    return 0;
}

int ExportPcap(const Options& o, Session& s) {
    uint64_t n = 0;
    if (!ch10::eth::WritePcap(s, o.channel, o.pcap_out, n)) { std::fprintf(stderr, "cannot write %s\n", o.pcap_out.c_str()); return 1; }
    std::printf("%llu frames written to %s\n", static_cast<unsigned long long>(n), o.pcap_out.c_str());
    return 0;
}

int ListMessages(const Options& o, Session& s) {
    ch10::Reader rd(s.path());
    ch10::Packet p;
    std::vector<ch10::Message> msgs;
    ch10::MessageCsdw csdw;
    long count = 0;
    for (uint32_t i : s.ByTime()) {
        const auto& e = s.packets()[i];
        if (e.data_type != ch10::kMessageF0) continue;
        if (o.channel >= 0 && e.channel != o.channel) continue;
        if (!rd.ReadAt(static_cast<std::streamoff>(e.offset), p)) continue;
        if (!ch10::DecodeMessageF0(p.Body(), p.BodyLen(), p.header.flags, p.header.rtc, csdw, msgs)) continue;
        for (const auto& m : msgs) {
            if (o.max_msgs >= 0 && count >= o.max_msgs) return 0;
            ++count;
            std::printf("#%-5ld ch%-3d sub%-3u %s  %zu B  %s%s\n", count, e.channel, m.subchannel,
                        s.TimeString(m.rtc).c_str(), m.data.size(), m.LooksLikeText() ? "" : "(binary) ",
                        m.Text().c_str());
        }
    }
    std::printf("-- %ld messages --\n", count);
    return 0;
}

int ExportWav(const Options& o, Session& s) {
    int channel = o.channel;
    if (channel < 0) {
        for (const auto& c : s.channels()) if (c.data_type == ch10::kAnalogF1) { channel = c.channel_id; break; }
        if (channel < 0) for (const auto& c : s.channels()) if (s.IsVideoType(c.data_type)) { channel = c.channel_id; break; }
    }
    const ch10::ChannelStats* cs = s.Channel(channel);
    if (!cs) { std::fprintf(stderr, "no such channel %d\n", channel); return 1; }

    std::unique_ptr<ch10::AudioSource> src;
    if (cs->data_type == ch10::kAnalogF1) {
        auto a = std::make_unique<ch10::AnalogAudioSource>();
        if (!a->Open(s, channel, o.subchannel, true, o.wav_rate)) { std::fprintf(stderr, "%s\n", a->error().c_str()); return 1; }
        src = std::move(a);
    } else if (cs->data_type == ch10::kMilStd1553F1) {
        ch10::Ms1553AudioSpec spec;
        spec.channel = channel;
        if (!o.m1553.rts.empty()) spec.rt = *o.m1553.rts.begin();
        if (!o.m1553.sas.empty()) spec.sa = *o.m1553.sas.begin();
        spec.tr = o.m1553.tr;
        spec.first_word = o.first_word; spec.word_count = o.word_count;
        spec.sample_rate = o.wav_rate;
        spec.format = o.audio_fmt == "u16" ? ch10::Ms1553AudioSpec::U16 :
                      o.audio_fmt == "s8x2" ? ch10::Ms1553AudioSpec::S8x2 :
                      o.audio_fmt == "u8x2" ? ch10::Ms1553AudioSpec::U8x2 :
                      o.audio_fmt == "ulaw" ? ch10::Ms1553AudioSpec::MuLawx2 :
                      o.audio_fmt == "alaw" ? ch10::Ms1553AudioSpec::ALawx2 : ch10::Ms1553AudioSpec::S16;
        auto m = std::make_unique<ch10::Ms1553AudioSource>();
        if (!m->Open(s, spec)) { std::fprintf(stderr, "%s\n", m->error().c_str()); return 1; }
        src = std::move(m);
    } else if (s.IsVideoType(cs->data_type)) {
        auto t = std::make_unique<ch10::TsAudioSource>();
        if (!t->Open(s, channel)) { std::fprintf(stderr, "%s\n", t->error().c_str()); return 1; }
        src = std::move(t);
    } else {
        std::fprintf(stderr, "channel %d is not an analog, 1553 or video channel\n", channel); return 1;
    }
    uint64_t from = src->first_rtc(), to = src->last_rtc();
    if (o.from_s >= 0) from = s.rtc_min() + static_cast<uint64_t>(o.from_s * 1e7);
    if (o.to_s >= 0)   to   = s.rtc_min() + static_cast<uint64_t>(o.to_s * 1e7);
    int rate = o.wav_rate > 0 ? o.wav_rate : static_cast<int>(src->sample_rate() + 0.5);
    if (!ch10::WriteWav(*src, from, to, rate, o.wav_out)) { std::fprintf(stderr, "cannot write %s\n", o.wav_out.c_str()); return 1; }
    std::printf("%s: %s\n  %.3f s at %d Hz -> %s\n", src->name().c_str(), src->info().c_str(),
                (to - from) / 1e7, rate, o.wav_out.c_str());
    return 0;
}

int ListAnnotations(const Session& s) {
    std::printf("Annotations (%zu):\n", s.annotations().size());
    for (const auto& a : s.annotations()) {
        const char* src = a.source == ch10::AnnotationSource::File ? "file" :
                          a.source == ch10::AnnotationSource::Event ? "event" : "user";
        std::printf("  %s  %s%s%s  [%s/%s] ch%d  %s\n", s.RelativeString(a.rtc_start).c_str(),
                    s.TimeString(a.rtc_start).c_str(),
                    a.IsRange() ? " -> " : "", a.IsRange() ? s.RelativeString(a.rtc_end).c_str() : "",
                    ch10::SeverityName(a.severity), src, a.channel, a.text.c_str());
    }
    std::printf("Anomalies (%llu detected, %zu listed):\n",
                static_cast<unsigned long long>(s.anomaly_total()), s.anomalies().size());
    for (const auto& a : s.anomalies())
        std::printf("  %s  ch%-3d pkt %-8llu %s\n", s.RelativeString(a.rtc_start).c_str(),
                    a.channel, static_cast<unsigned long long>(a.packet_index + 1), a.text.c_str());
    return 0;
}

void PrintOpResult(const ch10::OpResult& r) {
    for (const auto& o : r.outputs) std::printf("  wrote %s\n", o.string().c_str());
    std::printf("  %llu packets in, %llu packets out, %llu bytes\n",
                static_cast<unsigned long long>(r.packets_in),
                static_cast<unsigned long long>(r.packets_out),
                static_cast<unsigned long long>(r.bytes_out));
    if (!r.warning.empty()) std::printf("  note: %s\n", r.warning.c_str());
}

int DoCut(const Options& o, Session& s) {
    ch10::FilterSpec spec;
    spec.channels = o.keep_channels;
    if (o.drop_video) { spec.drop_types.insert(ch10::kVideoF0); spec.drop_types.insert(ch10::kVideoF1); }
    if (o.from_s >= 0) spec.rtc_from = s.rtc_min() + static_cast<uint64_t>(o.from_s * 1e7);
    if (o.to_s >= 0)   spec.rtc_to   = s.rtc_min() + static_cast<uint64_t>(o.to_s * 1e7);
    spec.m1553 = o.m1553;
    std::vector<ch10::Annotation> extra = o.annotate;
    for (auto& a : extra) { a.rtc_start += s.rtc_min(); a.rtc_end += s.rtc_min(); }
    ch10::Injection inj;
    if (!o.inject_file.empty()) {
        ch10::UserDataParse up = ch10::UserData::ParseFile(o.inject_file, s);
        for (const auto& e : up.errors) std::fprintf(stderr, "%s: %s\n", o.inject_file.c_str(), e.c_str());
        if (up.records.empty()) { std::fprintf(stderr, "no records to inject\n"); return 1; }
        std::printf("  injecting %d notes, %d text messages, %d 1553 messages\n", up.notes, up.texts, up.messages1553);
        inj = ch10::UserData::ToInjection(up.records, s);
    }
    ch10::OpResult r;
    if (!ch10::Cut(s, spec, extra, inj, o.cut_out, r)) { std::fprintf(stderr, "%s\n", r.error.c_str()); return 1; }
    PrintOpResult(r);
    return 0;
}

int DoSplit(const Options& o, Session& s) {
    ch10::OpResult r;
    if (!ch10::Split(s, o.split_every, static_cast<uint64_t>(o.split_mb * 1048576.0),
                     o.split_prefix, r)) {
        std::fprintf(stderr, "%s\n", r.error.c_str()); return 1;
    }
    PrintOpResult(r);
    return 0;
}

int DoMerge(const Options& o) {
    std::vector<ch10::MergeInput> in;
    in.push_back({o.file, 0});
    for (const auto& f : o.merge_files) in.push_back({f, 0});
    if (o.channel_offset >= 0) {
        for (std::size_t i = 1; i < in.size(); ++i)
            in[i].channel_offset = o.channel_offset * static_cast<int>(i);
    } else {
        std::vector<std::unique_ptr<Session>> tmp;
        std::vector<Session*> ptrs;
        for (const auto& m : in) {
            tmp.push_back(std::make_unique<Session>());
            if (!tmp.back()->Open(m.path)) {
                std::fprintf(stderr, "%s: %s\n", m.path.string().c_str(), tmp.back()->error().c_str());
                return 1;
            }
            ptrs.push_back(tmp.back().get());
        }
        auto offs = ch10::SuggestChannelOffsets(ptrs);
        for (std::size_t i = 0; i < in.size(); ++i) in[i].channel_offset = offs[i];
    }
    for (std::size_t i = 0; i < in.size(); ++i)
        std::printf("  input %zu: %s (channel offset +%d)\n", i + 1,
                    in[i].path.string().c_str(), in[i].channel_offset);
    ch10::OpResult r;
    if (!ch10::Merge(in, o.merge_out, r)) { std::fprintf(stderr, "%s\n", r.error.c_str()); return 1; }
    PrintOpResult(r);
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    Options o;
    // Template writers need no input file.
    if (argc >= 3 && (std::string(argv[1]) == "--inject-template" || std::string(argv[1]) == "--wordmap-template")) {
        o.template_out = std::string(std::string(argv[1]) == "--inject-template" ? "i:" : "w:") + argv[2];
    } else if (!ParseArgs(argc, argv, o)) {
        Usage();
        return 2;
    }

    if (!o.merge_out.empty()) return DoMerge(o);
    if (!o.template_out.empty()) {
        bool inj = o.template_out[0] == 'i';
        std::string path = o.template_out.substr(2);
        std::FILE* f = std::fopen(path.c_str(), "wb");
        if (!f) { std::fprintf(stderr, "cannot write %s\n", path.c_str()); return 1; }
        std::string t = inj ? ch10::UserData::Template() : ch10::WordMap::ExampleCsv();
        std::fwrite(t.data(), 1, t.size(), f); std::fclose(f);
        std::printf("template written to %s\n", path.c_str());
        return 0;
    }

    Session s;
    if (!s.Open(o.file)) {
        std::fprintf(stderr, "%s\n", s.error().c_str());
        return 1;
    }
    if (!o.cut_out.empty())      return DoCut(o, s);
    if (!o.split_prefix.empty()) return DoSplit(o, s);
    if (o.do_annotations)        return ListAnnotations(s);
    if (!o.wav_out.empty())      return ExportWav(o, s);
    if (o.do_ethernet)           return ListEthernet(o, s);
    if (!o.pcap_out.empty())     return ExportPcap(o, s);
    if (o.do_messages)           return ListMessages(o, s);

    if (o.do_tmats)      return DumpTmats(o, s);
    if (o.do_1553)       return Dump1553(o, s);
    if (o.do_video)      return ExtractVideo(o, s);
    if (o.do_snapshot)   return Snapshot(o, s);
    PrintSummary(s);
    return 0;
}
