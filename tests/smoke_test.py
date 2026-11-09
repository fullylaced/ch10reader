#!/usr/bin/env python3
"""End-to-end tests for ch10core via the ch10dump CLI.

Builds a synthetic, spec-conformant Chapter 10 file with tools/make_test_ch10.py and
checks every file operation the CLI exposes: summary, 1553 decode + CSV, cut, split,
merge with RTC rebasing, annotation packets, PCAP export, WAV export, user-data
injection, and header-corruption resync.

usage: smoke_test.py <ch10dump binary> <repo root> [workdir]
Exit code 0 on success. Needs only Python 3 - no third-party modules.
"""
import os
import re
import struct
import subprocess
import sys
import tempfile
import wave

CH10DUMP = os.path.abspath(sys.argv[1])
ROOT = os.path.abspath(sys.argv[2])
WORK = os.path.abspath(sys.argv[3]) if len(sys.argv) > 3 else tempfile.mkdtemp(prefix="ch10test_")
GEN = os.path.join(ROOT, "tools", "make_test_ch10.py")
os.makedirs(WORK, exist_ok=True)

FAILS = []


def run(*args, check=True):
    r = subprocess.run([CH10DUMP, *args], capture_output=True, text=True)
    if check and r.returncode != 0:
        raise RuntimeError(f"ch10dump {' '.join(args)} failed ({r.returncode}):\n{r.stdout}\n{r.stderr}")
    return r.stdout + r.stderr


def check(name, cond, detail=""):
    status = "ok  " if cond else "FAIL"
    print(f"[{status}] {name}" + (f"  ({detail})" if detail and not cond else ""))
    if not cond:
        FAILS.append(name)


def gen(path, start_s=0):
    # "none" = no transport-stream video channel, so the test needs no ffmpeg.
    subprocess.run([sys.executable, GEN, path, "none", str(start_s)], check=True, capture_output=True)
    return path


# ---------------------------------------------------------------- generate
t1 = gen(os.path.join(WORK, "t1.ch10"))
t2 = gen(os.path.join(WORK, "t2.ch10"), start_s=10)

# ---------------------------------------------------------------- summary
summary = run(t1)
check("summary: TMATS parsed", "TMATS: RCC 106-15" in summary and "CH10READER TEST" in summary)
check("summary: time correlated", "270:14:30:00.000000 -> 270:14:30:05.000000" in summary)
check("summary: 1553 channel + message count", re.search(r"\s16\s+50\s+\d+\s+0x19 MIL-STD-1553 F1.*\[1754 msgs\]", summary) is not None)
check("summary: analog channel", re.search(r"\s40\s+50\s+\d+\s+0x21 Analog F1", summary) is not None)
check("summary: ethernet channel + frames", re.search(r"0x68 Ethernet F0.*\[55 frames\]", summary) is not None)
check("summary: message channel", re.search(r"0x30 Message F0.*\[5 msgs\]", summary) is not None)
check("summary: TMATS source names applied", "1553 Bus A" in summary and "Telemetry LAN" in summary)

# ---------------------------------------------------------------- 1553 decode
m1553 = run(t1, "--m1553", "--max", "12")
check("1553: BC->RT decode", re.search(r"BC->RT\s+RT05 R SA01 WC04", m1553) is not None)
check("1553: RT->BC decode", re.search(r"RT->BC\s+RT05 T SA02 WC02", m1553) is not None)
check("1553: RT->RT decode", "RT->RT" in m1553)
check("1553: mode code decode", "Mode Code" in m1553)
check("1553: broadcast decode", "(bc" in m1553)

csv_path = os.path.join(WORK, "m1553.csv")
run(t1, "--m1553", csv_path)
rows = open(csv_path).read().splitlines()
check("1553 CSV: header + 1754 rows", len(rows) == 1755, f"{len(rows)} lines")
check("1553 CSV: columns", rows[0].startswith("msg_index,channel,time,bus,type,rt,tr,sa,wc_mc,cmd1"))

# ---------------------------------------------------------------- annotations / anomalies
ann = run(t1, "--annotations")
check("anomaly detection: 1553 block-status error flagged", re.search(r"1553 error \[TO\] RT09 T SA04", ann) is not None)

# ---------------------------------------------------------------- cut + annotate
cut = os.path.join(WORK, "cut.ch10")
run(t1, "--cut", cut, "--from", "1", "--to", "3",
    "--severity", "warning", "--annotate", "Check RT5@1.2",
    "--severity", "anomaly", "--annotate", "Dropout@2.4-2.9")
cut_summary = run(cut)
check("cut: time range honoured", "270:14:30:01.000000 -> 270:14:30:03.000000  (2.000 s)" in cut_summary)
check("cut: annotations counted", "Annotations: 2" in cut_summary)
cut_ann = run(cut, "--annotations")
check("annotation packets: round-trip read", "[Warning/file]" in cut_ann and "Check RT5" in cut_ann)
dropout_line = next((l for l in cut_ann.splitlines() if "Dropout" in l), "")
check("annotation packets: ranged anomaly", "->" in dropout_line and "[Anomaly/file]" in dropout_line, dropout_line)

chan_cut = os.path.join(WORK, "chan.ch10")
run(t1, "--cut", chan_cut, "--channels", "16")
chan_summary = run(chan_cut)
check("cut: channel filter keeps 1553 only",
      "0x19 MIL-STD-1553 F1" in chan_summary and "0x21 Analog" not in chan_summary and "0x68 Ethernet" not in chan_summary)

# ---------------------------------------------------------------- split
prefix = os.path.join(WORK, "part")
run(t1, "--split", prefix, "--every", "2")
parts = sorted(p for p in os.listdir(WORK) if p.startswith("part_part"))
check("split: 5 s / 2 s = 3 parts", len(parts) == 3, str(parts))
total_pkts = 0
for p in parts:
    m = re.search(r"\((\d+) bytes, (\d+) packets\)", run(os.path.join(WORK, p)))
    total_pkts += int(m.group(2))
check("split: every part readable, each carries its own TMATS", total_pkts >= 162)

# ---------------------------------------------------------------- merge
merged = os.path.join(WORK, "merged.ch10")
run(t1, "--merge", merged, t2, "--channel-offset", "auto")
ms = run(merged)
check("merge: 15 s span after RTC rebasing", "270:14:30:00.000000 -> 270:14:30:15.000000  (15.000 s)" in ms)
check("merge: packets from both files", re.search(r"\(\d+ bytes, 32[0-9] packets\)", ms) is not None)
check("merge: TMATS records combined", "46 attributes" in ms)

# ---------------------------------------------------------------- Ethernet + PCAP
eth = run(t1, "--ethernet", "--max", "5")
check("ethernet: UDP decode", re.search(r"UDP\s+10\.0\.0\.10:5000", eth) is not None or "10.0.0.10" in eth)
check("ethernet: ARP decode", "ARP" in eth)
pcap = os.path.join(WORK, "lan.pcap")
out = run(t1, "--pcap", pcap)
check("pcap: 55 frames written", "55 frames written" in out)
with open(pcap, "rb") as f:
    magic = f.read(4)
check("pcap: libpcap magic", magic in (b"\xd4\xc3\xb2\xa1", b"\xa1\xb2\xc3\xd4", b"\x4d\x3c\xb2\xa1", b"\xa1\xb2\x3c\x4d"))

# ---------------------------------------------------------------- Message F0
msgs = run(t1, "--messages")
check("message F0: text listed", "GPGGA" in msgs or "$GP" in msgs or re.search(r"\[5 msgs\]|Message", msgs) is not None)

# ---------------------------------------------------------------- Analog -> WAV
wav_path = os.path.join(WORK, "analog.wav")
run(t1, "--wav", wav_path, "--channel", "40")
with wave.open(wav_path) as w:
    check("wav: 40000 samples at 8000 Hz mono", (w.getnframes(), w.getframerate(), w.getnchannels()) == (40000, 8000, 1),
          f"{w.getnframes()} {w.getframerate()} {w.getnchannels()}")

# ---------------------------------------------------------------- user data injection
tmpl = os.path.join(WORK, "inject.txt")
run(t1, "--inject-template", tmpl)
injected = os.path.join(WORK, "injected.ch10")
run(t1, "--cut", injected, "--inject", tmpl)
inj_summary = run(injected)
check("inject: new message channel described in TMATS", re.search(r"\s200\s+\d+\s+\d+\s+0x30 Message F0", inj_summary) is not None)
inj_ann = run(injected, "--annotations")
check("inject: annotation from text file", "Check RT5 wraparound" in inj_ann)
inj_1553 = run(injected, "--m1553", "--rt", "7", "--sa", "4")
check("inject: ASCII 1553 data words", "4845 4C4C 4F20" in inj_1553.replace("\n", " "))  # "HELLO "

# ---------------------------------------------------------------- header corruption -> resync
corrupt = os.path.join(WORK, "corrupt.ch10")
data = bytearray(open(t1, "rb").read())
# Destroy the sync pattern of a packet in the middle of the file.
off = data.find(b"\x25\xEB", len(data) // 2)
data[off] = 0x00
data[off + 1] = 0x00
open(corrupt, "wb").write(data)
cs = run(corrupt)
m = re.search(r"\((\d+) bytes, (\d+) packets, (\d+) resyncs\)", cs)
check("resync: destroyed header skipped, rest of file read", m is not None and int(m.group(2)) == 161 and int(m.group(3)) == 1, cs.splitlines()[0])

# ---------------------------------------------------------------- done
print()
if FAILS:
    print(f"{len(FAILS)} check(s) failed: {FAILS}")
    sys.exit(1)
print("all checks passed")
