import argparse
from bisect import bisect_right
from collections import Counter, defaultdict
import json
import os
from pathlib import Path
import re
import struct
import subprocess


def jit_symbols(path):
    data = path.read_bytes()
    if len(data) < 40:
        raise ValueError("Truncated JIT dump header")
    magic, version, size, machine, padding, pid, timestamp, flags = struct.unpack_from(
        "<IIIIIIQQ", data
    )
    if magic != 0x4A695444 or version != 1 or size < 40 or size > len(data) or flags:
        raise ValueError("Unsupported JIT dump header or clock")
    offset = size
    pages = defaultdict(list)
    while offset < len(data):
        if offset + 16 > len(data):
            raise ValueError("Truncated JIT dump record")
        kind, length, stamp = struct.unpack_from("<IIQ", data, offset)
        if length < 16 or offset + length > len(data):
            raise ValueError("Invalid JIT dump record size")
        if kind == 0:
            if length < 57:
                raise ValueError("Truncated JIT load record")
            process, thread, vma, address, count, index = struct.unpack_from(
                "<IIQQQQ", data, offset + 16
            )
            end = data.index(b"\0", offset + 56, offset + length)
            if end + 1 + count > offset + length:
                raise ValueError("Truncated JIT machine code")
            name = "JIT::" + data[offset + 56 : end].decode(errors="replace")
            for page in range(address // 4096, (address + count - 1) // 4096 + 1):
                pages[page].append((stamp, address, address + count, name))
        elif kind == 1:
            raise ValueError("JIT code moves require a move-aware decoder")
        offset += length
    for entries in pages.values():
        entries.sort(reverse=True)
    return pages


def resolve_jit(pages, address, timestamp, symbol):
    if symbol == "[unknown]":
        for created, start, end, name in pages.get(address // 4096, []):
            if created <= timestamp and start <= address < end:
                return name
    return symbol


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("directory", type=Path)
    parser.add_argument("--perf", type=Path, required=True)
    args = parser.parse_args()
    run = json.loads((args.directory / "run.json").read_text())
    windows = []
    if "clockCorrelation" in run:
        clock = run["clockCorrelation"]
        offset = (
            run["timeOrigin"]
            + clock["monotonicMilliseconds"]
            - clock["wallMilliseconds"]
        )
        windows = sorted(
            (
                offset + item["startTime"],
                offset + item["startTime"] + item["duration"],
                item["name"],
            )
            for item in run.get("timings", [])
            if item["name"].endswith(("-sync", "-async", "-mutation", "-layout"))
        )
    starts = [item[0] for item in windows]
    heads = re.compile(r"^\s*(.*?)\s+(\d+)/(\d+)\s+(\d+\.\d+):\s*$")
    frames = re.compile(r"^\s+([a-f0-9]+)\s+(.*)\s+\((.*)\)$")
    mappings = re.compile(
        r"PERF_RECORD_MMAP2? (\d+)/\d+: .* (/.*?/jit-[^/]+\.dump)\s*$"
    )
    loaded_jit = {}
    process_jit = {}
    inclusive = defaultdict(Counter)
    self_counts = defaultdict(Counter)
    totals = Counter()
    depths = defaultdict(Counter)
    group = None
    stack = []

    def finish():
        if group:
            for name in (("all-main",) if group == "all-main" else ("all-main", group)):
                totals[name] += 1
                depths[name][len(stack)] += 1
                self_counts[name][stack[0] if stack else "<no-callchain>"] += 1
                inclusive[name].update(set(stack))

    environment = os.environ.copy()
    environment["LD_LIBRARY_PATH"] = str(args.perf.resolve().parents[1] / "lib")
    command = [
        str(args.perf),
        "script",
        "-i",
        str(args.directory / "samples.perf"),
        "--ns",
        "--show-mmap-events",
        "-F",
        "comm,pid,tid,time,ip,sym,dso",
    ]
    with (args.directory / "decode.log").open("w") as errors:
        process = subprocess.Popen(
            command, env=environment, stdout=subprocess.PIPE, stderr=errors, text=True
        )
        for line in process.stdout:
            mapping = mappings.search(line)
            if mapping:
                path = args.directory / Path(mapping[2]).name
                if path.is_file():
                    if path not in loaded_jit:
                        loaded_jit[path] = jit_symbols(path)
                    process_jit[mapping[1]] = loaded_jit[path]
                continue
            head = heads.match(line)
            if head:
                finish()
                stack = []
                group = None
                comm, pid, tid, timestamp = head.groups()
                if comm != "WPEWebProcess" or pid != tid:
                    continue
                group = "all-main"
                seconds, nanoseconds = timestamp.split(".")
                sample_time = int(seconds) * 1000000000 + int(
                    nanoseconds.ljust(9, "0")[:9]
                )
                stamp = float(timestamp) * 1000
                index = bisect_right(starts, stamp) - 1
                if index >= 0 and stamp <= windows[index][1]:
                    group = windows[index][2]
            elif group:
                frame = frames.match(line)
                if frame:
                    stack.append(
                        resolve_jit(
                            process_jit.get(pid, {}),
                            int(frame[1], 16),
                            sample_time,
                            frame[2],
                        )
                    )
        finish()
        if process.wait():
            raise RuntimeError("perf script failed")
    if not totals:
        raise RuntimeError("No main-thread stacks decoded")
    result = {
        name: {
            "samples": count,
            "meanDepth": sum(depth * n for depth, n in depths[name].items()) / count,
            "depths": dict(depths[name]),
            "self": dict(self_counts[name].most_common()),
            "inclusive": dict(inclusive[name].most_common()),
        }
        for name, count in totals.items()
    }
    (args.directory / "functions.json").write_text(json.dumps(result, indent=2) + "\n")
    print(
        json.dumps(
            {
                name: {
                    key: value
                    for key, value in values.items()
                    if key in ("samples", "meanDepth")
                }
                for name, values in result.items()
            },
            indent=2,
        )
    )


if __name__ == "__main__":
    main()
