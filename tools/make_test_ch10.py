#!/usr/bin/env python3
"""Generate a small spec-conformant IRIG 106 Chapter 10 test file.

Layout (all per IRIG 106-15):
  - Computer-Generated F1 setup record (TMATS) on channel 0
  - Time F1 packet (IRIG-B, day-of-year) on channel 1
  - Two MIL-STD-1553 F1 packets on channel 16 containing BC->RT, RT->BC,
    RT-to-RT, mode-code, and broadcast transactions with 16-bit data checksums

Use this to smoke-test ch10dump until a real recorder file is available.
"""
import os
import struct
import sys

SYNC = 0xEB25
RTC_HZ = 10_000_000


def header(channel, data_len, seq, flags, dtype, rtc, body_and_filler_len):
    packet_len = 24 + body_and_filler_len
    h = struct.pack('<HHIIBBBB', SYNC, channel, packet_len, data_len,
                    0x06, seq, flags, dtype)
    h += struct.pack('<Q', rtc)[:6]                      # 48-bit RTC
    cksum = sum(struct.unpack('<11H', h)) & 0xFFFF       # 10.6.1.1.j
    return h + struct.pack('<H', cksum)


def finish_packet(channel, seq, flags, dtype, rtc, body):
    """Append filler + 16-bit data checksum, build header, return packet."""
    data_len = len(body)
    with_ck = body
    # pad body so that body+checksum lands on a 4-byte boundary
    while (len(with_ck) + 2) % 4:
        with_ck += b'\x00'
    ck = sum(struct.unpack('<%dH' % (len(with_ck) // 2), with_ck)) & 0xFFFF
    with_ck += struct.pack('<H', ck)
    flags |= 0x02                                        # 16-bit data checksum
    return header(channel, data_len, seq, flags, dtype, rtc,
                  len(with_ck)) + with_ck


def tmats_packet(rtc):
    text = (
        "G\\106:15;\n"
        "G\\PN:CH10READER TEST;\n"
        "G\\DSI\\N:1;\n"
        "G\\DSI-1:TESTDATA;\n"
        "R-1\\ID:TESTREC;\n"
        "R-1\\N:6;\n"
        "R-1\\DSI-1:TimeInternal;\n"
        "R-1\\TK1-1:1;\n"
        "R-1\\CDT-1:TIMEIN;\n"
        "R-1\\DSI-2:1553 Bus A;\n"
        "R-1\\TK1-2:16;\n"
        "R-1\\CDT-2:1553IN;\n"
        "R-1\\DSI-3:Cockpit Camera;\n"
        "R-1\\TK1-3:32;\n"
        "R-1\\CDT-3:VIDIN;\n"
        "R-1\\DSI-4:Intercom;\n"
        "R-1\\TK1-4:40;\n"
        "R-1\\CDT-4:ANAIN;\n"
        "R-1\\ASR-4-1:8000;\n"
        "R-1\\DSI-5:Telemetry LAN;\n"
        "R-1\\TK1-5:48;\n"
        "R-1\\CDT-5:ETHIN;\n"
        "R-1\\DSI-6:GPS Serial;\n"
        "R-1\\TK1-6:50;\n"
        "R-1\\CDT-6:MSGIN;\n"
    )
    csdw = struct.pack('<I', 0x0B)                       # ASCII, RCC 106-15
    return finish_packet(0, 0, 0x00, 0x01, rtc, csdw + text.encode())


def bcd(v):
    return ((v // 10) << 4) | (v % 10)


def time_packet(rtc, day, hh, mm, ss, ms, seq=0):
    csdw = struct.pack('<I', (0 << 9) | (0x0 << 4) | 0x1)  # day fmt, IRIG-B, ext
    w1 = (bcd(ss) << 8) | bcd(ms // 10)                  # TSn Sn | Hmn Tmn
    w2 = (bcd(hh) << 8) | bcd(mm)
    w3 = (bcd(day // 10) << 4) | (day % 10)
    w3 |= ((day // 100) << 8)
    w3 = ((day // 100) << 8) | (bcd(day % 100))
    body = csdw + struct.pack('<HHH', w1, w2, w3)
    return finish_packet(1, seq & 0xFF, 0x00, 0x11, rtc, body)


def cmd(rt, tr, sa, wc):
    return (rt << 11) | (tr << 10) | (sa << 5) | (wc & 0x1F)


def status(rt, **flags):
    w = rt << 11
    if flags.get('busy'):
        w |= 1 << 3
    return w


def msg_1553(rtc, bsw, words, gap1=90, gap2=0):
    ipts = struct.pack('<Q', rtc & 0xFFFFFFFFFFFF)
    ipdh = struct.pack('<HHH', bsw, (gap2 << 8) | gap1, len(words) * 2)
    return ipts + ipdh + struct.pack('<%dH' % len(words), *words)


def m1553_packet(channel, seq, rtc, messages):
    csdw = struct.pack('<I', len(messages) & 0xFFFFFF)   # TTB=00
    body = csdw + b''.join(messages)
    return finish_packet(channel, seq, 0x00, 0x19, rtc, body)


def ts_first_pcr_seconds(data):
    for i in range(len(data) // 188):
        p = data[i * 188:(i + 1) * 188]
        afc = (p[3] >> 4) & 0x3
        if afc in (2, 3) and p[4] >= 7 and (p[5] & 0x10):
            base = (p[6] << 25) | (p[7] << 17) | (p[8] << 9) | (p[9] << 1) | (p[10] >> 7)
            ext = ((p[10] & 1) << 8) | p[11]
            return (base * 300 + ext) / 27_000_000.0
    return 0.0


def ts_pcr_seconds(data):
    """Per-TS-packet time from the PCR clock (27 MHz), carried forward for
    packets without a PCR. Returns a list of seconds relative to the first PCR."""
    n = len(data) // 188
    times, last = [], None
    for i in range(n):
        p = data[i * 188:(i + 1) * 188]
        afc = (p[3] >> 4) & 0x3
        if afc in (2, 3) and p[4] >= 7 and (p[5] & 0x10):
            base = (p[6] << 25) | (p[7] << 17) | (p[8] << 9) | (p[9] << 1) | (p[10] >> 7)
            ext = ((p[10] & 1) << 8) | p[11]
            last = (base * 300 + ext) / 27_000_000.0
        times.append(last)
    first = next(t for t in times if t is not None)
    return [((t if t is not None else first) - first) for t in times]


def video_packets_from_ts(channel, ts_path, t0, ts_per_packet=100, srs=True):
    """Wrap a real MPEG-2 transport stream into Video F0 packets. Each Chapter
    10 packet is stamped with the RTC matching the PCR of its first TS packet.
    With srs=True the RTC *is* the PCR clock (RTC = PCR / 2.7), i.e. the
    recorder's SCR was RTC-locked (CSDW SRS=1); t0 is ignored then."""
    data = open(ts_path, 'rb').read()
    n_ts = len(data) // 188
    secs = ts_pcr_seconds(data)
    packets = []
    seq = 0
    for start in range(0, n_ts, ts_per_packet):
        chunk = data[start * 188:(start + ts_per_packet) * 188]
        rtc = t0 + int(secs[start] * RTC_HZ)
        csdw = struct.pack('<I', (1 << 29) if srs else 0)   # F0: SRS, no IPH, no BA
        packets.append(finish_packet(channel, seq & 0xFF, 0x00, 0x40, rtc,
                                     csdw + chunk))
        seq += 1
    return b''.join(packets), n_ts


def udp_frame(src_mac, dst_mac, src_ip, dst_ip, src_port, dst_port, payload):
    """Build a raw IEEE 802.3 frame with IPv4/UDP (no FCS)."""
    import socket
    udp_len = 8 + len(payload)
    udp = struct.pack('>HHHH', src_port, dst_port, udp_len, 0) + payload
    ip_len = 20 + len(udp)
    ip = bytearray(struct.pack('>BBHHHBBH4s4s', 0x45, 0, ip_len, 0x1234, 0x4000, 64, 17, 0,
                               socket.inet_aton(src_ip), socket.inet_aton(dst_ip)))
    csum = sum(struct.unpack('>10H', bytes(ip)))
    while csum >> 16:
        csum = (csum & 0xFFFF) + (csum >> 16)
    ip[10:12] = struct.pack('>H', (~csum) & 0xFFFF)
    return dst_mac + src_mac + b'\x08\x00' + bytes(ip) + udp


def ethernet_packets(channel, t0, duration_s, period_ms=100):
    """Ethernet F0 (10.6.15.1): each packet holds the frames of one 100 ms
    slice: a UDP telemetry line (ASCII) plus, once a second, an ARP request."""
    out, seq, n_frames = b'', 0, 0
    fc = b'\x02\x00\x00\x00\x00\x10'
    gs = b'\x02\x00\x00\x00\x00\x20'
    n_slices = int(duration_s * 1000 / period_ms)
    for i in range(n_slices):
        t = t0 + i * period_ms * (RTC_HZ // 1000)
        frames = []
        line = ('TLM,%.1f,ALT=%d,VEL=%d,STATE=%s' % (i * period_ms / 1000.0, i * 12, 40 + i,
                'PAD' if i < 10 else 'BOOST' if i < 25 else 'COAST')).encode()
        frames.append((t, udp_frame(fc, gs, '10.0.0.10', '10.0.0.20', 5000, 6000, line)))
        if i % 10 == 0:
            arp = (b'\xff' * 6 + fc + b'\x08\x06' + b'\x00\x01\x08\x00\x06\x04\x00\x01' +
                   fc + b'\x0a\x00\x00\x0a' + b'\x00' * 6 + b'\x0a\x00\x00\x14')
            frames.append((t + 5000, arp))
        body = struct.pack('<I', len(frames))                  # FORMAT=0, TTB=0
        for frtc, fr in frames:
            fid = (2 << 24) | (0 << 16) | len(fr)                # speed 100 Mbps, net 0, full MAC frame
            body += struct.pack('<Q', frtc & 0xFFFFFFFFFFFF) + struct.pack('<I', fid) + fr
            if len(fr) & 1:
                body += b'\x00'
        out += finish_packet(channel, seq & 0xFF, 0x00, 0x68, t, body)
        seq += 1
        n_frames += len(frames)
    return out, n_frames


def message_packets(channel, t0, duration_s):
    """Message Data F0 (10.6.9.1): one GPS-style text line per second."""
    out = b''
    for sec in range(int(duration_s)):
        t = t0 + sec * RTC_HZ
        text = ('$GPGGA,1430%02d.00,3352.1234,N,11753.5678,W,1,08,0.9,%d.0,M,,,*4A' % (sec, 100 + sec * 30)).encode()
        body = struct.pack('<I', 1) + struct.pack('<Q', t & 0xFFFFFFFFFFFF) + struct.pack('<I', len(text)) + text
        if len(text) & 1:
            body += b'\x00'
        out += finish_packet(channel, sec & 0xFF, 0x00, 0x30, t, body)
    return out


def analog_packets(channel, t0, duration_s, rate=8000, tone_hz=660, per_packet=800):
    """Analog F1: one subchannel, 16-bit unpacked LSB-padded (mode 01),
    `per_packet` samples per packet, packet header RTC = first sample."""
    import math
    csdw = (1 << 28) | (1 << 16) | (0 << 8) | (16 << 2) | 0x1   # SAME, TOTCHAN=1, 16 bits, mode 01
    out, seq, n_total = b'', 0, int(duration_s * rate)
    for start in range(0, n_total, per_packet):
        n = min(per_packet, n_total - start)
        samples = [int(12000 * math.sin(2 * math.pi * tone_hz * (start + i) / rate)) for i in range(n)]
        body = struct.pack('<I', csdw) + struct.pack('<%dh' % n, *samples)
        rtc = t0 + int(start / rate * RTC_HZ)
        out += finish_packet(channel, seq & 0xFF, 0x00, 0x21, rtc, body)
        seq += 1
    return out, n_total


def main(path, ts_path=None, start_s=0, rtc_base_s=None):
    """start_s: seconds after 14:30:00 the recording begins (for merge tests);
    rtc_base_s: the free-running RTC value at the start, in seconds. When not
    given and a TS is present, the RTC is the TS PCR clock (SRS recorder)."""
    import os
    duration = 5.0
    srs = rtc_base_s is None
    if srs and ts_path and os.path.exists(ts_path):
        rtc_base_s = ts_first_pcr_seconds(open(ts_path, 'rb').read())
    elif rtc_base_s is None:
        rtc_base_s = 5
    t0 = int(rtc_base_s * RTC_HZ)                        # RTC at recording start
    ms = RTC_HZ // 1000

    out = tmats_packet(t0)
    # Time F1 at 1 Hz (spec minimum) starting 14:30:00 (+start_s) on day 270.
    for sec in range(int(duration) + 1):
        tod = start_s + sec
        out += time_packet(t0 + sec * RTC_HZ, day=270, hh=14, mm=30 + tod // 60,
                           ss=tod % 60, ms=0, seq=sec)

    # MIL-STD-1553 on channel 16: a 50 Hz BC->RT frame with a counter in
    # word 1, an RT->BC reply, plus the special cases in the first packet.
    special = [
        msg_1553(t0 + 15 * ms, bsw=0x0800,                # RT-to-RT (bit 11)
                 words=[cmd(7, 0, 3, 2), cmd(5, 1, 2, 2),
                        status(5), 0xC0DE, 0xFACE, status(7)], gap2=80),
        msg_1553(t0 + 20 * ms, bsw=0x0000,                # tx mode code 17 w/ data
                 words=[cmd(5, 1, 31, 17), status(5), 0x0123]),
        msg_1553(t0 + 22 * ms, bsw=0x0000,                # broadcast receive
                 words=[cmd(31, 0, 1, 2), 0xDEAD, 0xBEEF], gap1=0),
        msg_1553(t0 + 25 * ms, bsw=0x0200,                # response timeout
                 words=[cmd(9, 1, 4, 1)], gap1=0),
    ]
    msgs, seq, pkt_start = [], 0, t0
    n_frames = int(duration * 50)
    import math
    audio_rate, audio_tone = 8000, 1000
    for i in range(n_frames):
        t = t0 + i * 20 * ms
        bus = 0x2000 if i % 2 else 0x0000                 # alternate A/B
        msgs.append(msg_1553(t, bsw=bus,
                             words=[cmd(5, 0, 1, 4), i & 0xFFFF, 0x1000 + i,
                                    0x2000 + (i * 3) & 0xFFFF, 0x4444, status(5)]))
        msgs.append(msg_1553(t + 2 * ms, bsw=bus,
                             words=[cmd(5, 1, 2, 2), status(5),
                                    0xAAAA, (i * 7) & 0xFFFF]))
        # RT10 SA5 carries 16-bit PCM audio: 5 messages of 32 samples per
        # 20 ms frame = 160 samples / 20 ms = 8 kHz, a 1 kHz tone.
        for k in range(5):
            base = i * 160 + k * 32
            pcm = [int(10000 * math.sin(2 * math.pi * audio_tone * (base + j) / audio_rate)) & 0xFFFF
                   for j in range(32)]
            msgs.append(msg_1553(t + k * 4 * ms, bsw=0x0000,
                                 words=[cmd(10, 0, 5, 0)] + pcm + [status(10)]))
        if i == 0:
            msgs.extend(special)
        if len(msgs) >= 35 or i == n_frames - 1:          # ~100 ms per packet
            out += m1553_packet(16, seq & 0xFF, pkt_start, msgs)
            msgs, seq, pkt_start = [], seq + 1, t + 20 * ms

    # Analog F1 on channel 40: 8 kHz 16-bit intercom audio (660 Hz tone).
    ana, n_ana = analog_packets(40, t0, duration)
    out += ana

    # Ethernet F0 on channel 48 (UDP telemetry + ARP) and Message F0 on 50 (GPS text).
    ethp, n_eth = ethernet_packets(48, t0, duration)
    out += ethp
    out += message_packets(50, t0, duration)

    # Video F0 on channel 32 from a real MPEG-2 TS if available.
    n_ts = 0
    if ts_path and os.path.exists(ts_path):
        vid, n_ts = video_packets_from_ts(32, ts_path, t0, srs=srs)
        out += vid

    with open(path, 'wb') as f:
        f.write(out)
    print(f'wrote {path}: {len(out)} bytes, {n_frames * 7 + 4} 1553 messages, '
          f'{n_ts} TS packets, {n_ana} analog samples, {n_eth} ethernet frames')


if __name__ == '__main__':
    here = os.path.dirname(os.path.abspath(__file__))
    main(sys.argv[1] if len(sys.argv) > 1 else 'test.ch10',
         sys.argv[2] if len(sys.argv) > 2 else os.path.join(here, 'testpattern.ts'),
         int(sys.argv[3]) if len(sys.argv) > 3 else 0,
         float(sys.argv[4]) if len(sys.argv) > 4 else None)
