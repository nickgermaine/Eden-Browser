import argparse
import json
import os
from pathlib import Path
import time

parser = argparse.ArgumentParser()
parser.add_argument("--group", type=int, required=True)
parser.add_argument("--output", type=Path, required=True)
args = parser.parse_args()


def snapshot():
    cpu = [int(value) for value in Path("/proc/stat").read_text().splitlines()[0].split()[1:9]]
    processes = {}
    for entry in Path("/proc").iterdir():
        if not entry.name.isdigit():
            continue
        try:
            contents = (entry / "stat").read_text()
            name = contents[contents.index("(") + 1 : contents.rindex(")")]
            fields = contents[contents.rindex(")") + 2 :].split()
            processes[int(entry.name)] = (
                name,
                int(fields[2]),
                int(fields[11]) + int(fields[12]),
                int(fields[19]),
            )
        except (OSError, ValueError):
            continue
    return time.time(), sum(cpu), sum(cpu) - cpu[3] - cpu[4], processes


before = snapshot()
with args.output.open("x") as stream:
    while Path(f"/proc/{args.group}").exists():
        time.sleep(1)
        after = snapshot()
        total = after[1] - before[1]
        own = 0
        background = []
        for pid, (name, group, ticks, start) in after[3].items():
            previous = before[3].get(pid)
            if not previous or previous[3] != start:
                continue
            delta = ticks - previous[2]
            if group in (args.group, os.getpgrp()):
                own += delta
            elif delta:
                background.append({"pid": pid, "name": name, "machineCpuPercent": 100 * delta / total})
        background.sort(key=lambda item: item["machineCpuPercent"], reverse=True)
        row = {
            "wallTime": after[0],
            "seconds": after[0] - before[0],
            "machineBusyPercent": 100 * (after[2] - before[2]) / total,
            "estimatedBackgroundPercent": 100 * max(0, after[2] - before[2] - own) / total,
            "topBackgroundProcesses": background[:8],
        }
        stream.write(json.dumps(row) + "\n")
        stream.flush()
        before = after
