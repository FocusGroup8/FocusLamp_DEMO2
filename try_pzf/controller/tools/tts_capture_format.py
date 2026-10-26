"""Lossless packet capture -> Ogg Opus -> optional PCM16 WAV.

The board's FOP1 export is a packet list, not concatenated Opus/Ogg audio.
Ogg granules always use 48kHz, independently of the WAV output sample rate.
No cloud credentials, network calls or speech synthesis are used here.
"""
from __future__ import annotations

import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import wave

MAX_EXPORT_BYTES = 512 * 1024 + 16


def parse_capture(data: bytes) -> tuple[int, list[bytes]]:
    if len(data) < 16 or len(data) > MAX_EXPORT_BYTES:
        raise ValueError("Invalid capture length")
    magic, rate, count, size = struct.unpack_from("<4sIII", data)
    if magic != b"FOP1" or rate != 16000 or count == 0 or size != len(data) - 16:
        raise ValueError("Invalid capture header or truncated download")
    packets = []
    pos = 16
    while pos < len(data):
        if pos + 2 > len(data):
            raise ValueError("Truncated packet length")
        n = struct.unpack_from("<H", data, pos)[0]
        pos += 2
        if n == 0 or pos + n > len(data):
            raise ValueError("Invalid or truncated Opus packet")
        packets.append(data[pos:pos + n])
        pos += n
    if len(packets) != count:
        raise ValueError("Packet count mismatch")
    return rate, packets


def opus_samples_48k(packet: bytes) -> int:
    """Duration from the Opus TOC (RFC 6716), not a fixed 60ms assumption."""
    if not packet:
        raise ValueError("Empty Opus packet")
    toc = packet[0]
    if toc & 0x80:
        per_frame = (48000 << ((toc >> 3) & 3)) // 400
    elif (toc & 0x60) == 0x60:
        per_frame = 960 if toc & 8 else 480
    else:
        shift = (toc >> 3) & 3
        per_frame = 2880 if shift == 3 else (48000 << shift) // 100
    code = toc & 3
    if code == 0:
        count = 1
    elif code in (1, 2):
        count = 2
    else:
        if len(packet) < 2:
            raise ValueError("Missing Opus frame count")
        count = packet[1] & 0x3F
    samples = per_frame * count
    if count == 0 or samples > 5760:
        raise ValueError("Invalid Opus duration")
    return samples


def ogg_crc(data: bytes) -> int:
    value = 0
    for octet in data:
        value ^= octet << 24
        for _ in range(8):
            value = ((value << 1) ^ (0x04C11DB7 if value & 0x80000000 else 0)) & 0xFFFFFFFF
    return value


def ogg_page(packet: bytes, sequence: int, granule: int, flags: int, serial: int) -> bytes:
    laces = [255] * (len(packet) // 255) + [len(packet) % 255]
    if len(laces) > 255:
        raise ValueError("Opus packet exceeds single-page capture format")
    header = struct.pack("<4sBBQIIIB", b"OggS", 0, flags, granule, serial, sequence, 0, len(laces))
    page = bytearray(header + bytes(laces) + packet)
    struct.pack_into("<I", page, 22, ogg_crc(page))
    return bytes(page)


def make_ogg(packets: list[bytes], input_rate: int = 16000, serial: int = 0x464F4355) -> bytes:
    if not packets:
        raise ValueError("No Opus packets")
    # Pre-skip 0 intentionally matches the board's real-time decode: no
    # original encoder lookahead/end-trim metadata is provided by this stream.
    head = struct.pack("<8sBBHIhB", b"OpusHead", 1, 1, 0, input_rate, 0, 0)
    vendor = b"FocusLamp cloud TTS capture"
    tags = b"OpusTags" + struct.pack("<I", len(vendor)) + vendor + struct.pack("<I", 0)
    pages = [ogg_page(head, 0, 0, 2, serial), ogg_page(tags, 1, 0, 0, serial)]
    granule = 0
    for index, packet in enumerate(packets):
        granule += opus_samples_48k(packet)
        pages.append(ogg_page(packet, index + 2, granule, 4 if index == len(packets) - 1 else 0, serial))
    return b"".join(pages)


def find_ffmpeg(explicit: str | None = None) -> str | None:
    candidate = explicit or os.environ.get("FFMPEG_BINARY") or shutil.which("ffmpeg")
    if candidate:
        resolved = shutil.which(candidate) or (str(Path(candidate).resolve()) if Path(candidate).is_file() else None)
        if not resolved:
            raise ValueError("FFmpeg path does not exist: " + candidate)
        return resolved
    # Optional dependency is private to this tool, not installed system-wide.
    deps = Path(__file__).resolve().parent / ".tts_capture_deps"
    if deps.is_dir():
        sys.path.insert(0, str(deps))
    try:
        import imageio_ffmpeg
        return imageio_ffmpeg.get_ffmpeg_exe()
    except ImportError:
        return None


def convert_wav(ffmpeg: str, opus_path: Path, wav_path: Path, expected_samples: int | None = None) -> float:
    """Create a new file only; never overwrite existing project assets."""
    if wav_path.exists():
        raise FileExistsError(wav_path)
    temporary = wav_path.with_suffix(".wav.part")
    try:
        subprocess.run([ffmpeg, "-hide_banner", "-loglevel", "error", "-nostdin", "-xerror", "-n",
                        "-i", str(opus_path), "-ac", "1", "-ar", "16000", "-c:a", "pcm_s16le",
                        "-f", "wav", str(temporary)], check=True, timeout=60)
        with wave.open(str(temporary), "rb") as audio:
            if (audio.getnchannels(), audio.getsampwidth(), audio.getframerate()) != (1, 2, 16000) or not audio.getnframes():
                raise ValueError("Decoded WAV is not nonempty PCM16 mono 16kHz")
            if expected_samples is not None and audio.getnframes() != expected_samples:
                raise ValueError("Decoded WAV duration differs from captured Opus packets")
            duration = audio.getnframes() / audio.getframerate()
        # Use exclusive destination creation: another process cannot be silently
        # overwritten between the earlier existence check and final publication.
        with wav_path.open("xb") as output, temporary.open("rb") as source:
            shutil.copyfileobj(source, output)
        return duration
    finally:
        temporary.unlink(missing_ok=True)
