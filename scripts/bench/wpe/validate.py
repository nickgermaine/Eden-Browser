import argparse
import base64
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from PIL import Image, ImageChops, ImageStat


def compare(left, right, location="geometry"):
    if isinstance(left, (int, float)) and isinstance(right, (int, float)):
        if abs(left - right) > 1 / 64:
            raise AssertionError(f"{location}: {left} != {right}")
    elif isinstance(left, list) and isinstance(right, list):
        if len(left) != len(right):
            raise AssertionError(f"{location}: changed line or glyph range count")
        for index, (a, b) in enumerate(zip(left, right)):
            compare(a, b, f"{location}[{index}]")
    elif left != right:
        raise AssertionError(f"{location}: {left!r} != {right!r}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--font-file", type=Path, action="append", default=[])
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[3]
    args.output.mkdir(parents=True, exist_ok=False)
    environment = os.environ.copy()
    for name in list(environment):
        if name.startswith(
            ("JSC_", "WEBKIT_", "EDEN_WPE_", "SYSPROF_", "LLVM_PROFILE")
        ):
            environment.pop(name)
    environment["WEBKIT_SKIA_CANVAS_REPLAY_THREADS"] = "2"
    fonts = [
        {
            "family": "FixtureVariable",
            "data": base64.b64encode(
                (root / "resources/fonts/RobotoFlex.ttf").read_bytes()
            ).decode(),
            "descriptors": {"weight": "100 1000"},
        },
        {
            "family": "FixtureRegularOnly",
            "data": base64.b64encode(
                (
                    root
                    / "third_party/wpe/source/wpewebkit-2.53.92-engine-experiments/Source/ThirdParty/pdfjs/web/standard_fonts/LiberationSans-Regular.ttf"
                ).read_bytes()
            ).decode(),
            "descriptors": {},
        },
    ]
    for index, path in enumerate(args.font_file):
        fonts.append(
            {
                "family": f"FixtureExtra{index}",
                "data": base64.b64encode(path.read_bytes()).decode(),
                "descriptors": {},
            }
        )
    records = []
    raster_records = []
    with tempfile.TemporaryDirectory(prefix="eden-text-validation-") as temporary:
        stage = Path(temporary)
        probe = stage / "probe.js"
        probe.write_text(
            "globalThis.edenFixtureFonts = "
            + json.dumps(fonts)
            + ";\n"
            + (root / "tests/wpe-text-shaping.js").read_text()
        )
        for scale in (1, 2):
            values = []
            for enabled in ("0", "1"):
                label = f"text-{enabled}-scale-{scale}"
                result = stage / "result.json"
                command = [
                    sys.executable,
                    str(root / "scripts/test_wpe.py"),
                    "--build",
                    str(args.build.resolve()),
                    "--scale",
                    str(scale),
                    "--benchmark",
                    "--",
                    "--output",
                    str(result),
                    "--label",
                    label,
                    "--probe-script",
                    str(probe),
                ]
                with (args.output / f"{label}.log").open("w") as log:
                    subprocess.run(
                        command,
                        env=environment | {"WEBKIT_EDEN_SIMPLE_TEXT": enabled},
                        stdout=log,
                        stderr=subprocess.STDOUT,
                        check=True,
                        timeout=1830,
                    )
                contents = result.read_bytes()
                (args.output / f"{label}.json").write_bytes(contents)
                values.append(json.loads(contents)["measurements"]["geometry"])
            compare(*values)
            records.append(
                {
                    "scale": scale,
                    "cases": len(values[0]),
                    "maximumCoordinateTolerance": 1 / 64,
                    "passed": True,
                }
            )
            print("Text geometry passed", records[-1], flush=True)
        for scale in (1, 2):
            pictures = []
            for mode, limit in [("gpu", "0"), ("worker-cpu", "1048576")]:
                label = f"canvas-{mode}-scale-{scale}"
                result = stage / "result.json"
                command = [
                    sys.executable,
                    str(root / "scripts/test_wpe.py"),
                    "--build",
                    str(args.build.resolve()),
                    "--scale",
                    str(scale),
                    "--benchmark",
                    "--",
                    "--output",
                    str(result),
                    "--label",
                    label,
                    "--probe-script",
                    str(root / "tests/wpe-canvas-raster.js"),
                ]
                with (args.output / f"{label}.log").open("w") as log:
                    subprocess.run(
                        command,
                        env=environment | {"WEBKIT_EDEN_CANVAS_CPU_PIXELS": limit},
                        stdout=log,
                        stderr=subprocess.STDOUT,
                        check=True,
                        timeout=360,
                    )
                contents = result.read_bytes()
                (args.output / f"{label}.json").write_bytes(contents)
                values = json.loads(contents)["measurements"]["canvases"]
                decoded = []
                for item in values:
                    if item["background"] != [20, 40, 60, 255]:
                        raise AssertionError(f"{label}: canvas background changed")
                    png = base64.b64decode(item["png"].split(",", 1)[1])
                    (args.output / f"{label}-{item['kind']}.png").write_bytes(png)
                    picture = Image.open(io.BytesIO(png)).convert("RGBA")
                    if picture.size != (512, 256) or picture.getchannel(
                        "A"
                    ).getextrema() != (255, 255):
                        raise AssertionError(
                            f"{label}: canvas dimensions or opacity changed"
                        )
                    decoded.append((item["kind"], picture))
                pictures.append(decoded)
            for (left_name, left), (right_name, right) in zip(*pictures, strict=True):
                if left_name != right_name:
                    raise AssertionError("Canvas fixture cases changed")
                difference = ImageChops.difference(left, right)
                changed = sum(any(pixel) for pixel in difference.getdata())
                raster_records.append(
                    {
                        "scale": scale,
                        "case": left_name,
                        "changedPixelPercent": changed / (512 * 256) * 100,
                        "channelRmsDifference": ImageStat.Stat(difference).rms,
                    }
                )
            print("Canvas quality measured", scale, flush=True)
    native_records = []
    for scale in (1, 2):
        for mode, settings in [
            (
                "text-and-damage",
                {"WEBKIT_EDEN_SIMPLE_TEXT": "1", "WEBKIT_EDEN_CANVAS_DAMAGE": "1"},
            ),
            (
                "cpu-raster",
                {
                    "WEBKIT_EDEN_CANVAS_CPU_PIXELS": "1048576",
                    "WEBKIT_EDEN_CANVAS_DAMAGE": "1",
                },
            ),
        ]:
            label = f"{mode}-scale-{scale}"
            command = [
                sys.executable,
                str(root / "scripts/test_wpe.py"),
                "--build",
                str(args.build.resolve()),
                "--scale",
                str(scale),
            ]
            if mode == "cpu-raster":
                command += [
                    "canvasReplayPreservesPixelsAndReadbackOrder",
                    "canvasReplayMatchesImmediateGpuPainting",
                    "canvasReplayPreservesSnapshotsAcrossFrames",
                    "canvasReplayPresentsAndSurvivesPendingDestruction",
                ]
            with (args.output / f"{label}.log").open("w") as log:
                completed = subprocess.run(
                    command,
                    env=environment | settings,
                    stdout=log,
                    stderr=subprocess.STDOUT,
                    check=False,
                    timeout=360,
                )
            native_records.append({"label": label, "returnCode": completed.returncode})
            print("Native checks finished", label, completed.returncode, flush=True)
    (args.output / "summary.json").write_text(
        json.dumps(
            {
                "textGeometry": records,
                "canvasQuality": raster_records,
                "nativeChecks": native_records,
            },
            indent=2,
        )
        + "\n"
    )
    if any(record["returnCode"] for record in native_records):
        raise RuntimeError("Native checks failed; see summary.json and native logs")


if __name__ == "__main__":
    main()
