#!/usr/bin/env python3
"""Compare generated and delivered stereo S16 PCM from one bounded run."""
import argparse
import json
from pathlib import Path


def signal_and_gaps(path):
    pcm = path.read_bytes()
    if len(pcm) % 4:
        raise ValueError(f'{path}: incomplete stereo frame')
    signal, gaps, gap = bytearray(), {}, 0
    for offset in range(0, len(pcm), 4):
        frame = pcm[offset:offset + 4]
        if frame == b'\0\0\0\0':
            gap += 1
            continue
        if gap:
            gaps[len(signal) // 4] = gap
            gap = 0
        signal.extend(frame)
    return signal, gaps


def compare(generated, delivered):
    source, source_gaps = signal_and_gaps(generated)
    output, output_gaps = signal_and_gaps(delivered)
    count = len(output) // 4
    matched = bool(count) and len(output) <= len(source) and output == source[:len(output)]
    added = missing = 0
    if matched:
        for index in source_gaps.keys() | output_gaps.keys():
            # Initial priming and the unplayed tail aren't steady playback.
            if not 0 < index < count:
                continue
            delta = output_gaps.get(index, 0) - source_gaps.get(index, 0)
            added += max(0, delta)
            missing += max(0, -delta)
    return dict(signal_matches=matched, delivered_signal_frames=count,
                added_silence_frames=added, missing_silence_frames=missing,
                passed=matched and added == 0 and missing == 0)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('generated', type=Path)
    parser.add_argument('delivered', type=Path)
    args = parser.parse_args()
    result = compare(args.generated, args.delivered)
    print(json.dumps(result, indent=2))
    return 0 if result['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
