// ch10/wordmap.h
// User-supplied names and scaling for MIL-STD-1553 data words, so raw words
// display as readable engineering values. Loaded from a CSV:
//
//   channel,rt,sa,tr,word,name,type,scale,offset,units
//   *,5,1,R,0,frame_counter,u16,1,0,
//   *,5,1,R,1,altitude,s16,0.5,0,m
//   *,5,1,R,2,status,hex,,,
//   *,5,1,R,3,callsign,ascii:4,,,        <- 4 words = 8 characters
//   16,10,5,*,0,temperature,f32,1,0,degC  <- two words, IEEE float (hi word first)
//
// '*' matches anything; tr is R or T; word is the zero-based data word index.
// types: u16 s16 u32 s32 f32 hex bin ascii[:N]

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ch10 {

struct WordMapEntry {
    int channel = -1, rt = -1, sa = -1, tr = -1;   // -1 = any
    int word = 0;
    std::string name, type, units;
    double scale = 1.0, offset = 0.0;
    int ascii_words = 1;
};

struct DecodedWord {
    int         word = 0;
    std::string name, value, units;
};

class WordMap {
public:
    bool LoadCsv(const std::string& path, std::string* error = nullptr);
    bool ParseCsv(const std::string& text, std::string* error = nullptr);
    bool empty() const { return entries_.empty(); }
    std::size_t size() const { return entries_.size(); }
    const std::vector<WordMapEntry>& entries() const { return entries_; }

    // Decode the data words of one message.
    std::vector<DecodedWord> Decode(int channel, int rt, int sa, bool transmit,
                                    const std::vector<uint16_t>& data) const;

    static std::string ExampleCsv();

private:
    std::vector<WordMapEntry> entries_;
};

// Words as text: each 16-bit word holds two characters (high byte first).
std::string WordsToAscii(const std::vector<uint16_t>& words, std::size_t first = 0,
                         std::size_t count = std::string::npos);
std::vector<uint16_t> AsciiToWords(const std::string& text);

} // namespace ch10
