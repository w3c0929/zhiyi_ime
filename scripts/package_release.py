"""Assembles a release folder (and a zip) from a build plus a reference installation.

  python scripts/package_release.py --reference "D:\\scoop\\...\\1.5.4.xxxx" \
      --out dist --version 1.6.0

The build tree holds only the binaries; the data (dictionaries, themes, ui strings), the model
folders, the runtime DLLs (onnxruntime, DirectML, the MSVC runtimes) and the licenses live in an
installed copy. So this copies the reference installation, overwrites it with the freshly built
programs, and zips the result into dist/zhiyu-v<version>.zip.

It does not build the NSIS setup.exe: upstream's zhiyu-setup.nsi is not published here. Either
point makensis at your own installer script over the produced folder, or ship the zip. After the
setup.exe exists, run scripts/make_release.py to build and sign latest.json.
"""
import argparse
import ctypes
import json
import os
import re
import shutil
import sys
from ctypes import wintypes

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


class VsFixedFileInfo(ctypes.Structure):
    _fields_ = [
        ("dwSignature", wintypes.DWORD),
        ("dwStrucVersion", wintypes.DWORD),
        ("dwFileVersionMS", wintypes.DWORD),
        ("dwFileVersionLS", wintypes.DWORD),
        ("dwProductVersionMS", wintypes.DWORD),
        ("dwProductVersionLS", wintypes.DWORD),
        ("dwFileFlagsMask", wintypes.DWORD),
        ("dwFileFlags", wintypes.DWORD),
        ("dwFileOS", wintypes.DWORD),
        ("dwFileType", wintypes.DWORD),
        ("dwFileSubtype", wintypes.DWORD),
        ("dwFileDateMS", wintypes.DWORD),
        ("dwFileDateLS", wintypes.DWORD),
    ]


def pe_file_version(path):
    """The Windows version resource of a file, as (major, minor, patch, build), or None when it
    has none."""
    version = ctypes.windll.version
    size = version.GetFileVersionInfoSizeW(str(path), None)
    if not size:
        return None
    buffer = ctypes.create_string_buffer(size)
    if not version.GetFileVersionInfoW(str(path), 0, size, buffer):
        return None
    value = ctypes.c_void_p()
    length = wintypes.UINT()
    if not version.VerQueryValueW(buffer, "\\", ctypes.byref(value), ctypes.byref(length)):
        return None
    if length.value < ctypes.sizeof(VsFixedFileInfo):
        return None
    info = ctypes.cast(value, ctypes.POINTER(VsFixedFileInfo)).contents
    return (info.dwFileVersionMS >> 16, info.dwFileVersionMS & 0xFFFF,
            info.dwFileVersionLS >> 16, info.dwFileVersionLS & 0xFFFF)


def wrong_build_versions(out_dir, version):
    """Every program of ours in `out_dir` whose version resource is not `version` — (name, found).

    The version inside the programs is what Settings shows and what the update check compares
    against latest.json, so a package whose programs still say the previous version is one that
    offers to update to itself for ever. Measured: the first 1.7.56 package shipped 1.7.55.0
    programs, because `cmake --build` never picked the new VERSION up and nothing but the file the
    edit touched was rebuilt."""
    match = re.match(r"^(\d+)\.(\d+)\.(\d+)", version)
    if not match:
        return []
    expected = tuple(int(part) for part in match.groups())
    wrong = []
    for name in sorted(os.listdir(out_dir)):
        path = os.path.join(out_dir, name)
        if not name.lower().startswith("zhiyu") or not os.path.isfile(path):
            continue
        found = pe_file_version(path)
        if found is not None and found[:3] != expected:
            wrong.append((name, ".".join(str(part) for part in found)))
    return wrong


# Built programs, relative to the build tree; both architectures where they exist.
BUILT = {
    "x64": [
        (r"server\Release\zhiyu-server.exe", "zhiyu-server.exe"),
        (r"settings\Release\zhiyu-settings.exe", "zhiyu-settings.exe"),
        (r"tsf\Release\zhiyu_tsf_x64.dll", "zhiyu_tsf_x64.dll"),
        (r"legacy_ime\Release\zhiyu_ime_x64.ime", "zhiyu_ime_x64.ime"),
        (r"resource\Release\zhiyu-resources.dll", "zhiyu-resources.dll"),
        (r"installer\Release\zhiyu-installer-helper.exe", "zhiyu-installer-helper.exe"),
    ],
    "x86": [
        (r"tsf\Release\zhiyu_tsf_x86.dll", "zhiyu_tsf_x86.dll"),
        (r"legacy_ime\Release\zhiyu_ime_x86.ime", "zhiyu_ime_x86.ime"),
    ],
}


def copy_reference(reference, out_dir):
    shutil.copytree(reference, out_dir, dirs_exist_ok=True, ignore=shutil.ignore_patterns(
        "*.replaced", "uninstall.exe", "install-manifest.json", ".zhiyu-install-complete"))


def overlay_builds(reference, out_dir, build_x64, build_x86):
    written = []
    for arch, build in (("x64", build_x64), ("x86", build_x86)):
        if not build:
            continue
        for relative, name in BUILT[arch]:
            source = os.path.join(build, relative)
            if os.path.exists(source):
                shutil.copy2(source, os.path.join(out_dir, name))
                written.append(name)
            else:
                print(f"missing (skipped): {name} in {build}", file=sys.stderr)
    return written


# Text data the repository owns (ui strings, default config, themes, ...): the reference's copy
# is the upstream brand. Overwrite these with the repo's. Only the text config is listed — the
# repository also holds the dictionary *sources* (*.dict.db.zip) and build helpers, which must
# not go into the installer (they are already-compressed build inputs and just bloat it); the
# built .bin/.idx and the model folders stay from the reference.
DATA_OVERLAY = ("default.json", "ui.zh-CN.json", "ui.en-US.json", "themes.json",
                "ai_thinking_levels.json", "ai_actions.json",
                "punctuation.json", "symbols.json", "settings_presets.json",
                "english.words.tsv")


def overlay_repo_data(out_dir):
    source_dir = os.path.join(ROOT, "data")
    target_dir = os.path.join(out_dir, "data")
    written = []
    for name in DATA_OVERLAY:
        source = os.path.join(source_dir, name)
        if os.path.isfile(source):
            shutil.copy2(source, os.path.join(target_dir, name))
            written.append(name)
    return written


# The lifecycle (installer/src/installer_lifecycle_*.cc) keeps a list of a version directory's
# files; the installer copies this file into the version directory along with the rest.
MANIFEST_NAME = "install-manifest.json"
MANIFEST_SKIP = {MANIFEST_NAME, ".zhiyu-install-complete"}
# Written by the installer (WriteUninstaller) rather than shipped, but part of the version.
MANIFEST_EXTRA = ("uninstall.exe",)


def write_install_manifest(out_dir):
    files = []
    for root, _dirs, names in os.walk(out_dir):
        for name in names:
            if name in MANIFEST_SKIP:
                continue
            relative = os.path.relpath(os.path.join(root, name), out_dir)
            files.append(relative.replace(os.sep, "/"))
    files = sorted(set(files).union(MANIFEST_EXTRA))
    manifest = {"format": "zhiyu-install-manifest", "version": 1, "files": files}
    path = os.path.join(out_dir, MANIFEST_NAME)
    with open(path, "w", encoding="utf-8") as file:
        json.dump(manifest, file, ensure_ascii=False, indent=2)
        file.write("\n")
    return path


def main():
    parser = argparse.ArgumentParser(description="Assemble a release folder and zip.")
    parser.add_argument("--reference", required=True, help="an installed copy to take data from")
    parser.add_argument("--build", default=os.path.join(ROOT, "build-x64"))
    parser.add_argument("--x86-build", default=os.path.join(ROOT, "build-x86"))
    parser.add_argument("--out", default=os.path.join(ROOT, "dist"))
    parser.add_argument("--version", required=True)
    args = parser.parse_args()

    out_dir = os.path.join(args.out, f"zhiyu-v{args.version}")
    if os.path.exists(out_dir):
        shutil.rmtree(out_dir)
    os.makedirs(args.out, exist_ok=True)

    print(f"copying {args.reference} -> {out_dir}")
    copy_reference(args.reference, out_dir)
    print("overwriting built programs:")
    for name in overlay_builds(args.reference, out_dir, args.build, args.x86_build):
        print(f"  {name}")
    print("overwriting repository data:")
    for name in overlay_repo_data(out_dir):
        print(f"  data\\{name}")
    print(f"writing {write_install_manifest(out_dir)}")

    wrong = wrong_build_versions(out_dir, args.version)
    if wrong:
        for name, found in wrong:
            print(f"error: {name} is stamped {found} inside, not {args.version}", file=sys.stderr)
        print("error: the build did not pick VERSION up — configure again "
              f"(cmake -S . -B {os.path.basename(args.build)}) and rebuild before packaging",
              file=sys.stderr)
        return 1

    archive = os.path.join(args.out, f"zhiyu-v{args.version}")
    shutil.make_archive(archive, "zip", args.out, f"zhiyu-v{args.version}")
    print(f"\nfolder: {out_dir}\nzip:    {archive}.zip")
    print("\nNext: build the setup.exe (your NSIS script), then")
    print(f"  python scripts/make_release.py --version {args.version} --installer "
          f"<zhiyu-v{args.version}-setup.exe>")
    return 0


if __name__ == "__main__":
    sys.exit(main())
