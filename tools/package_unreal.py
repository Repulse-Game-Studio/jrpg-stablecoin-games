"""Build the Unreal release zips (`tools/package.py --engine unreal`): one per supported engine
version, dist/scfg-unreal-<version>-ue5.6.zip, -ue5.7.zip and -ue5.8.zip.

Each zip holds the plugin folder, which developers copy into their project's Plugins/ folder:

    Scfg/
        Scfg.uplugin                   (EngineVersion "5.x.0" for that zip; no test module entries)
        Source/ScfgCore/, Source/Scfg/, Source/ScfgEditor/   (source only)
        Resources/                     (the checkout page copy, Resources/Web/, without bridge.js)
        Config/FilterPlugin.ini        (keeps the three files below in a packaged plugin)
        README.md                      (Unreal developer guide, joined from docs/developers/unreal/)
        LICENSE                        (MIT license)
        NOTICES.txt                    (notices of the third-party code the core ports)

That is Fab's source layout (4.3.7.3.a): no Binaries/, Intermediate/ or other build output, and
every path 170 characters or less (4.3.7.3.c). Fab wants one zip per engine version, differing in
the .uplugin's "EngineVersion" (4.2.2.d); the zips hold the same source.

The test modules (ScfgCoreTests, ScfgTests) and their .uplugin entries are left out: ScfgCoreTests
needs unreal/core-tests/, which does not ship, and ScfgTests' hooks are for the plugin's own tests.
Files outside Source/, Resources/ and Content/ are dropped by RunUAT BuildPlugin (the package that
Fab builds) unless Config/FilterPlugin.ini lists them
(AutomationTool/Scripts/BuildPluginCommand.Automation.cs, FilterPluginFiles), hence that file.

The version is in two places in the zip, the .uplugin's VersionName and the core's exported
scfg::VERSION (Source/ScfgCore/Private/Version.cpp); both must equal scfg_core's __version__.
Resources/Web/bridge.js (the web exports' bridge, which sync_web_page.py keeps in every copy of the
page) is left out: the Unreal server never serves it and Scfg.Build.cs never stages it.

Every file is copied byte for byte, apart from Scfg.uplugin; the zips are reproducible (sorted
entries, fixed timestamps).
"""

from __future__ import annotations

import json
import re
import sys
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from package import ROOT, UNREAL_PAGES, __version__, developer_guide  # noqa: E402

PLUGIN = ROOT / "unreal" / "Plugins" / "Scfg"
ZIP_ROOT = "Scfg/"
ENGINE_VERSIONS = ("5.6", "5.7", "5.8")
SHIPPED_MODULES = ("ScfgCore", "Scfg", "ScfgEditor")
TEST_MODULES = ("ScfgCoreTests", "ScfgTests")
# Build output and editor state (Fab 4.3.7.3.a), wherever they appear under a shipped folder.
SKIP_PARTS = {"Binaries", "Intermediate", "Saved", "DerivedDataCache", "Build"}
MAX_PATH = 170  # Fab 4.3.7.3.c
# Files of the shipped folders that the zip leaves out, relative to the plugin folder.
NOT_SHIPPED = {"Resources/Web/bridge.js"}
VERSION_CPP = "Source/ScfgCore/Private/Version.cpp"
VERSION_LITERAL = re.compile(r'^const std::string_view VERSION = "([^"]*)";$', re.MULTILINE)
GUIDE_TITLE = "# Stablecoin for Games: Unreal Developer Guide"
FILTER_PLUGIN = """[FilterPlugin]
; The files beside Scfg.uplugin that a packaged copy of the plugin keeps (RunUAT BuildPlugin and
; Fab copy only Source/, Resources/ and Content/ by default): the guide, the license and the
; notices of the third-party code the core ports.
/README.md
/LICENSE
/NOTICES.txt
"""
ZIP_DATE = (1980, 1, 1, 0, 0, 0)


def zip_name(engine, version=__version__):
    return f"scfg-unreal-{version}-ue{engine}.zip"


def descriptor(plugin=PLUGIN):
    return json.loads((plugin / "Scfg.uplugin").read_text(encoding="utf-8"))


def core_constant_version(plugin=PLUGIN):
    """The literal scfg::VERSION is defined to in Version.cpp, or None if it has none."""
    found = VERSION_LITERAL.findall((plugin / VERSION_CPP).read_text(encoding="utf-8"))
    return found[0] if len(found) == 1 else None


def problems(plugin=PLUGIN, version=__version__):
    """What stops the zips: the .uplugin's version, the core's version constant, and modules the
    packager does not know."""
    out = []
    plugin_descriptor = descriptor(plugin)
    name = plugin_descriptor.get("VersionName")
    if name != version:
        out.append(
            f"Scfg.uplugin VersionName {name} != scfg_core {version} (the release bump sets both)"
        )
    constant = core_constant_version(plugin)
    if constant is None:
        out.append(f"no scfg::VERSION literal in {VERSION_CPP}")
    elif constant != name:
        out.append(f"scfg::VERSION {constant} (Version.cpp) != Scfg.uplugin VersionName {name}"
                   " (the release bump sets both)")
    modules = [m.get("Name") for m in plugin_descriptor.get("Modules", [])]
    known = set(SHIPPED_MODULES) | set(TEST_MODULES)
    out += [f"Scfg.uplugin module {m} is neither shipped nor a test module" for m in modules
            if m not in known]
    out += [f"Scfg.uplugin has no module {m}" for m in SHIPPED_MODULES if m not in modules]
    for folder in sorted(p.name for p in (plugin / "Source").iterdir() if p.is_dir()):
        if folder not in known:
            out.append(f"Source/{folder} is neither a shipped nor a test module")
    return out


def packaged_descriptor(engine, plugin=PLUGIN):
    """Scfg.uplugin as the zip for `engine` ("5.x") holds it: that EngineVersion, and no test
    modules."""
    out = descriptor(plugin)
    out["EngineVersion"] = f"{engine}.0"
    out["Modules"] = [m for m in out["Modules"] if m.get("Name") not in TEST_MODULES]
    return json.dumps(out, indent="\t", ensure_ascii=False) + "\n"


def files_to_ship(plugin=PLUGIN):
    """(path, name) for the plugin's own files, sorted, names relative to the plugin folder:
    Source/<shipped module>/ and Resources/, without build output, hidden files or NOT_SHIPPED."""
    roots = [plugin / "Source" / m for m in SHIPPED_MODULES] + [plugin / "Resources"]
    found = []
    for root in roots:
        for path in root.rglob("*"):
            rel = path.relative_to(plugin)
            if path.is_dir() or SKIP_PARTS & set(rel.parts):
                continue
            if any(part.startswith(".") for part in rel.parts) or path.name.endswith("~"):
                continue
            if rel.as_posix() in NOT_SHIPPED:
                continue
            found.append((path, rel.as_posix()))
    return sorted(found, key=lambda item: item[1])


def notices(plugin=PLUGIN):
    """The notices of the code the core ports, for NOTICES.txt.

    The Nayuki notice is read from QrCodeGen.h, where the port keeps it, so the two cannot drift.
    """
    header = plugin / "Source" / "ScfgCore" / "Public" / "Scfg" / "QrCodeGen.h"
    lines = header.read_text(encoding="utf-8").splitlines()
    start = lines.index("// QR Code generator library (Python)")
    end = next(i for i, line in enumerate(lines) if line.startswith("// Ported to C++"))
    qr = "\n".join(line[3:] if line.startswith("// ") else "" for line in lines[start:end])
    return (
        "Third-party code in the Scfg plugin\n"
        "===================================\n\n"
        "QR Code generator (Source/ScfgCore/Public/Scfg/QrCodeGen.h, Private/QrCodeGen.cpp)\n"
        "------------------------------------------------------------------------------\n"
        "A C++ port of Project Nayuki's QR Code generator library (Python). The original notice\n"
        "follows.\n\n"
        f"{qr.strip()}\n\n"
        "Keccak-256 (Source/ScfgCore/Private/Keccak.cpp)\n"
        "-----------------------------------------------\n"
        "A C++ port of XKCP's CompactFIPS202 sponge (Gilles Van Assche, https://keccak.team/).\n"
        "To the extent possible under law, the implementer has waived all copyright and related\n"
        "or neighboring rights to the source code (CC0,\n"
        "http://creativecommons.org/publicdomain/zero/1.0/).\n"
    )


def guide():
    """The Unreal developer guide (docs/developers/unreal/), joined like the Godot one: its
    README (the sidebar category's landing page) is the intro, the other pages sections."""
    return developer_guide(UNREAL_PAGES, (), GUIDE_TITLE, intro=True)


def entries(engine, plugin=PLUGIN):
    """(name in the zip, bytes) for the zip for `engine`, sorted by name."""
    generated = {
        "Scfg.uplugin": packaged_descriptor(engine, plugin).encode("utf-8"),
        "Config/FilterPlugin.ini": FILTER_PLUGIN.encode("utf-8"),
        "README.md": guide().encode("utf-8"),
        "LICENSE": (ROOT / "LICENSE").read_bytes(),
        "NOTICES.txt": notices(plugin).encode("utf-8"),
    }
    out = []
    for path, name in files_to_ship(plugin):
        if name in generated:
            raise ValueError(f"{path} would be overwritten by the generated {name}")
        out.append((ZIP_ROOT + name, path.read_bytes()))
    out += [(ZIP_ROOT + name, data) for name, data in generated.items()]
    return sorted(out)


def path_problems(names):
    return [f"{name} is longer than {MAX_PATH} characters ({len(name)})" for name in names
            if len(name) > MAX_PATH]


def build_unreal_zips(dist, plugin=PLUGIN, version=__version__):
    """dist/scfg-unreal-<version>-ue<5.x>.zip for each of ENGINE_VERSIONS (see the module
    docstring); the paths written."""
    found = problems(plugin, version)
    if found:
        raise ValueError("the Unreal plugin is not ready:\n  " + "\n  ".join(found))
    dist.mkdir(parents=True, exist_ok=True)
    targets = []
    for engine in ENGINE_VERSIONS:
        files = entries(engine, plugin)
        too_long = path_problems(name for name, _data in files)
        if too_long:
            raise ValueError(f"Fab allows paths of {MAX_PATH} characters at most:\n  "
                             + "\n  ".join(too_long))
        target = dist / zip_name(engine, version)
        with zipfile.ZipFile(target, "w", zipfile.ZIP_DEFLATED) as zf:
            for name, data in files:
                info = zipfile.ZipInfo(name, ZIP_DATE)
                info.compress_type = zipfile.ZIP_DEFLATED
                info.external_attr = 0o644 << 16
                zf.writestr(info, data)
        print(f"  {target.name}: {len(files)} files")
        targets.append(target)
    return targets


def main(dist, plugin=PLUGIN):
    """`tools/package.py --engine unreal`: build the zips into `dist`; 1, with the reason on
    stderr, when the plugin is not ready."""
    try:
        targets = build_unreal_zips(dist, plugin)
    except ValueError as error:
        print(f"package.py --engine unreal: {error}", file=sys.stderr)
        return 1
    for target in targets:
        print(f"wrote {target}")
    return 0
