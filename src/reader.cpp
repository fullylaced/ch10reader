// src/reader.cpp
#include "ch10/reader.h"

#include <cstring>

namespace ch10 {

Reader::Reader(const std::filesystem::path& path)
    : file_(path, std::ios::binary) {
    if (!file_.is_open()) {
        error_ = "cannot open file: " + path.string();
        return;
    }
    file_.seekg(0, std::ios::end);
    file_size_ = file_.tellg();
    file_.seekg(0, std::ios::beg);
}

void Reader::Rewind() {
    file_.clear();
    file_.seekg(0, std::ios::beg);
}

bool Reader::Next(Packet& out) {
    if (!file_.is_open()) return false;

    for (;;) {
        std::streamoff start = file_.tellg();
        if (start < 0 || start + static_cast<std::streamoff>(kHeaderSize) > file_size_)
            return false;

        uint8_t hdr[kHeaderSize];
        file_.read(reinterpret_cast<char*>(hdr), kHeaderSize);
        if (file_.gcount() != static_cast<std::streamsize>(kHeaderSize))
            return false;

        if (!HeaderLooksValid(hdr)) {
            // Corrupt or misaligned: rewind one byte past `start` and scan.
            file_.clear();
            file_.seekg(start + 1, std::ios::beg);
            ++resyncs_;
            if (resync_offsets_.size() < 1000) resync_offsets_.push_back(start);
            if (!Resync()) return false;
            continue;
        }

        PacketHeader h;
        ParseHeader(hdr, h);

        if (start + static_cast<std::streamoff>(h.packet_len) > file_size_) {
            // Truncated final packet.
            error_ = "truncated packet at end of file";
            return false;
        }

        out.header      = h;
        out.file_offset = start;
        out.raw.resize(h.packet_len);
        std::memcpy(out.raw.data(), hdr, kHeaderSize);
        file_.read(reinterpret_cast<char*>(out.raw.data() + kHeaderSize),
                   static_cast<std::streamsize>(h.packet_len - kHeaderSize));
        if (file_.gcount() !=
            static_cast<std::streamsize>(h.packet_len - kHeaderSize))
            return false;
        return true;
    }
}

bool Reader::ReadAt(std::streamoff offset, Packet& out) {
    if (!file_.is_open()) return false;
    file_.clear();
    file_.seekg(offset, std::ios::beg);
    return Next(out) && out.file_offset == offset;
}

bool Reader::Resync() {
    // Scan forward in chunks looking for 0xEB25 followed by a header whose
    // checksum and length fields validate.
    constexpr std::size_t kChunk = 1 << 16;
    std::vector<uint8_t> buf(kChunk + kHeaderSize);

    for (;;) {
        std::streamoff base = file_.tellg();
        if (base < 0 ||
            base + static_cast<std::streamoff>(kHeaderSize) > file_size_)
            return false;

        file_.read(reinterpret_cast<char*>(buf.data()),
                   static_cast<std::streamsize>(buf.size()));
        std::size_t got = static_cast<std::size_t>(file_.gcount());
        file_.clear();
        if (got < kHeaderSize) return false;

        for (std::size_t i = 0; i + kHeaderSize <= got; ++i) {
            if (buf[i] == 0x25 && buf[i + 1] == 0xEB &&
                HeaderLooksValid(buf.data() + i)) {
                file_.seekg(base + static_cast<std::streamoff>(i),
                            std::ios::beg);
                return true;
            }
        }
        // Overlap by header size so a sync spanning the chunk edge is found.
        file_.seekg(base + static_cast<std::streamoff>(got - kHeaderSize + 1),
                    std::ios::beg);
    }
}

} // namespace ch10
