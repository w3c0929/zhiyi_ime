"""Downloads ONNX Runtime (CPU build, Windows x64) into third_party/onnxruntime/.

  python scripts/fetch_onnxruntime.py

Everything that runs a model runs it on the processor (the recommendation model, SenseVoice;
the two sherpa models have their own ONNX Runtime in voice-runtime/), so the plain CPU package
is enough. The engine links onnxruntime.lib from there and the build copies onnxruntime.dll
next to zhiyu-server.exe.
  - ONNX Runtime: NuGet package Microsoft.ML.OnnxRuntime (MIT).
See THIRD_PARTY_NOTICES.txt.
"""
import io
import os
import shutil
import sys
import urllib.request
import zipfile

VERSION = "1.24.4"
NUGET = (f"https://api.nuget.org/v3-flatcontainer/microsoft.ml.onnxruntime/{VERSION}/"
         f"microsoft.ml.onnxruntime.{VERSION}.nupkg")
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEST = os.path.join(ROOT, "third_party", "onnxruntime")


def download(url):
    print(f"downloading {url}")
    with urllib.request.urlopen(url, timeout=300) as r:
        return r.read()


def main():
    marker = os.path.join(DEST, "VERSION_NUMBER")
    if (os.path.exists(marker) and open(marker).read().strip() == VERSION and
            os.path.exists(os.path.join(DEST, "lib", "onnxruntime.dll")) and
            not os.path.exists(os.path.join(DEST, "lib", "DirectML.dll"))):
        print(f"ONNX Runtime {VERSION} (CPU) already in {DEST}")
        return 0
    package = download(NUGET)

    shutil.rmtree(DEST, ignore_errors=True)
    files = {
        "build/native/include/": "include/",
        "runtimes/win-x64/native/onnxruntime.dll": "lib/onnxruntime.dll",
        "runtimes/win-x64/native/onnxruntime.lib": "lib/onnxruntime.lib",
        "LICENSE": "LICENSE",
        "ThirdPartyNotices.txt": "ThirdPartyNotices.txt",
    }
    with zipfile.ZipFile(io.BytesIO(package)) as z:
        for info in z.infolist():
            if info.is_dir():
                continue
            for source, target in files.items():
                if info.filename == source or (source.endswith("/") and info.filename.startswith(source)):
                    path = os.path.join(DEST, target + info.filename[len(source):] if source.endswith("/") else target)
                    os.makedirs(os.path.dirname(path), exist_ok=True)
                    with z.open(info) as src, open(path, "wb") as dst:
                        shutil.copyfileobj(src, dst)
    with open(marker, "w") as f:
        f.write(VERSION)
    print(f"ONNX Runtime {VERSION} (CPU) -> {DEST}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
