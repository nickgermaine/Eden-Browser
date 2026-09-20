import json
from pathlib import Path
import subprocess
import tempfile

BEGIN = "/*eden-inspector-begin*/"
END = "/*eden-inspector-end*/"
STYLESHEET = "/org/webkit/inspector/UserInterface/Main.css"
SCRIPT = "/org/webkit/inspector/UserInterface/Main.js"
SCRIPT_PATCHES = [
    (
        '"window-inactive":"window-docked-inactive",inactive)',
        '"window-inactive":"window-docked-inactive",false)',
    ),
]
IMAGES = "/org/webkit/inspector/UserInterface/Images/"
ICONS = {
    "Elements.svg": "Network, IT, Programming/Code.svg",
    "Console.svg": "Network, IT, Programming/Programming.svg",
    "Sources.svg": "Notes/Document Text.svg",
    "SourcesPaused.svg": "Video, Audio, Sound/Pause Circle.svg",
    "Network.svg": "Map & Location/Global.svg",
    "Timeline.svg": "Time/Stopwatch.svg",
    "Storage.svg": "Essentional, UI/Database.svg",
    "Graphics.svg": "Video, Audio, Sound/Gallery.svg",
    "Layers.svg": "Design, Tools/Layers.svg",
    "Audit.svg": "List/Checklist Minimalistic.svg",
    "Search.svg": "Search/Magnifer.svg",
    "Gear.svg": "Settings, Fine Tuning/Settings.svg",
    "Overflow.svg": "Arrows/Alt Arrow Down.svg",
}


EXTRA_IMAGES = {
    "EdenDock.svg": "resources/icons/solar/sidebar-minimalistic-linear.svg",
    "EdenWindow.svg": "resources/icons/solar/new-window-linear.svg",
    "EdenWindowed.svg": "resources/icons/solar/window-linear.svg",
    "EdenClose.svg": "resources/icons/solar/close-linear.svg",
}


def strip_theme(text):
    while BEGIN in text:
        start = text.index(BEGIN)
        end = text.index(END, start) + len(END)
        text = text[:start] + text[end:]
    return text


def apply_inspector_theme(root, prefix):
    resource = prefix / "share" / "wpe-webkit-2.0" / "inspector.gresource"
    if not resource.is_file():
        return False
    overrides = (root / "scripts" / "wpe" / "eden-inspector.css").read_text()
    overrides += (root / "scripts" / "wpe" / "inspector" / "eden-request-editor.css").read_text()
    editor = (root / "scripts" / "wpe" / "inspector" / "eden-request-editor.js").read_text()
    hook = (root / "scripts" / "wpe" / "inspector" / "eden-page-hook.js").read_text()
    editor = editor.replace("__EDEN_PAGE_HOOK__", json.dumps(hook))
    solar = root / "third_party" / "solar-icons" / "icons" / "SVG" / "Linear"
    names = subprocess.run(
        ["gresource", "list", str(resource)], check=True, capture_output=True, text=True
    ).stdout.split()
    if STYLESHEET not in names:
        return False
    with tempfile.TemporaryDirectory() as staging:
        staged = Path(staging)
        entries = []
        for name in names:
            data = subprocess.run(["gresource", "extract", str(resource), name], check=True, capture_output=True).stdout
            if name == STYLESHEET:
                data = (strip_theme(data.decode()) + BEGIN + overrides + END).encode()
            elif name == SCRIPT:
                text = data.decode()
                for before, after in SCRIPT_PATCHES:
                    if after not in text:
                        if text.count(before) != 1:
                            raise RuntimeError("The inspector script does not match the Eden theme patch")
                        text = text.replace(before, after, 1)
                data = (strip_theme(text) + BEGIN + "\n;" + editor + END).encode()
            elif name.startswith(IMAGES) and name[len(IMAGES):] in ICONS:
                replacement = solar / ICONS[name[len(IMAGES):]]
                if replacement.is_file():
                    data = replacement.read_bytes()
            target = staged / name.lstrip("/")
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
            entries.append(f'<file compressed="true">{name.lstrip("/")}</file>')
        for image, source in EXTRA_IMAGES.items():
            name = IMAGES + image
            target = staged / name.lstrip("/")
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes((root / source).read_bytes())
            entry = f'<file compressed="true">{name.lstrip("/")}</file>'
            if entry not in entries:
                entries.append(entry)
        manifest = staged / "inspector.gresource.xml"
        manifest.write_text(
            '<?xml version="1.0" encoding="UTF-8"?>\n<gresources>\n<gresource prefix="/">\n'
            + "\n".join(entries)
            + "\n</gresource>\n</gresources>\n"
        )
        output = staged / "inspector.gresource"
        subprocess.run(
            ["glib-compile-resources", f"--target={output}", f"--sourcedir={staged}", str(manifest)], check=True
        )
        resource.write_bytes(output.read_bytes())
    return True
