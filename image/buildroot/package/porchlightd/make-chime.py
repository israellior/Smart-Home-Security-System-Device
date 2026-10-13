#!/usr/bin/env python3
"""Write the doorbell's chime: a two-tone ding-dong, 48 kHz mono 16-bit WAV.

The same two tones provision.sh made with gst-launch on Raspberry Pi OS - 784 Hz
for 18 buffers, then 622 Hz for 32, at audiotestsrc's 1024 samples a buffer -
written here with the standard library, at build time, because the image has
no GStreamer and the file should not live in git as a binary. Same input, same
bytes, every build.

    make-chime.py OUT.wav
"""
import array
import math
import sys
import wave

RATE = 48000
TONES = ((784, 18 * 1024), (622, 32 * 1024))  # (Hz, samples)
LEVEL = 0.4 * 32767                            # well short of clipping
FADE = RATE // 100                              # 10 ms each end: no click


def tone(freq, count):
    samples = array.array("h")
    for n in range(count):
        envelope = min(1.0, n / FADE, (count - 1 - n) / FADE)
        samples.append(int(LEVEL * envelope * math.sin(2 * math.pi * freq * n / RATE)))
    return samples


def main(path):
    with wave.open(path, "wb") as out:
        out.setnchannels(1)
        out.setsampwidth(2)
        out.setframerate(RATE)
        for freq, count in TONES:
            data = tone(freq, count)
            if sys.byteorder != "little":
                data.byteswap()   # WAV is little-endian whatever builds it
            out.writeframes(data.tobytes())


if __name__ == "__main__":
    main(sys.argv[1])
