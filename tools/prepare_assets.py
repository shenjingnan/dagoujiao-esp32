#!/usr/bin/env python3
"""Extract the three locally saved browser samples into linker-friendly PCM."""

from __future__ import annotations

import argparse
import base64
import re
import struct
from pathlib import Path

NAMES = ("da", "gou", "jiao")


def wav_pcm(encoded: str) -> list[int]:
    blob = base64.b64decode(encoded)
    if blob[:4] != b"RIFF" or blob[8:12] != b"WAVE":
        raise ValueError("sample is not a RIFF/WAVE file")
    pos = 12
    channels = rate = bits = None
    data = None
    while pos + 8 <= len(blob):
        chunk_id = blob[pos : pos + 4]
        size = struct.unpack_from("<I", blob, pos + 4)[0]
        chunk = blob[pos + 8 : pos + 8 + size]
        if chunk_id == b"fmt ":
            fmt, channels, rate, _, _, bits = struct.unpack_from("<HHIIHH", chunk)
            if fmt != 1 or channels != 2 or rate != 32000 or bits != 16:
                raise ValueError(f"unsupported WAV: fmt={fmt}, ch={channels}, rate={rate}, bits={bits}")
        elif chunk_id == b"data":
            data = chunk
        pos += 8 + size + (size & 1)
    if data is None or channels is None:
        raise ValueError("WAV is missing fmt or data")
    stereo = struct.unpack("<" + "h" * (len(data) // 2), data)
    return [(stereo[i] + stereo[i + 1]) // 2 for i in range(0, len(stereo), 2)]


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--audio-data", type=Path, required=True)
    parser.add_argument("--out-c", type=Path, required=True)
    parser.add_argument("--out-h", type=Path, required=True)
    args = parser.parse_args()
    source = args.audio_data.read_text(encoding="utf-8")
    matches = dict(re.findall(r"^\s*([A-Za-z0-9_]+):\s*'([^']+)'", source, flags=re.MULTILINE))
    samples = {name: wav_pcm(matches[name]) for name in NAMES}
    args.out_h.write_text(
        "#pragma once\n#include <stdint.h>\n\n"
        "typedef struct { const int16_t *pcm; uint32_t frames; } bark_sample_t;\n"
        "extern const bark_sample_t k_bark_da, k_bark_gou, k_bark_jiao;\n",
        encoding="utf-8",
    )
    lines = ['#include "bark_samples.h"', '']
    for name, pcm in samples.items():
        lines.append(f"static const int16_t k_{name}_pcm[] = {{")
        for start in range(0, len(pcm), 12):
            lines.append("    " + ", ".join(str(value) for value in pcm[start : start + 12]) + ",")
        lines.append("};")
        lines.append(f"const bark_sample_t k_bark_{name} = {{k_{name}_pcm, {len(pcm)}}};")
        lines.append("")
    args.out_c.write_text("\n".join(lines), encoding="utf-8")


if __name__ == "__main__":
    main()
