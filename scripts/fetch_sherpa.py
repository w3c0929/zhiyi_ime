"""Downloads the sherpa-onnx Windows x64 runtime (C API) into third_party/sherpa-onnx/.

  python scripts/fetch_sherpa.py

The voice input's two LLM recognisers (Fun-ASR-Nano, Qwen3-ASR) run in a small worker
process (voice_worker/) that links sherpa-onnx. It is kept out of zhiyu-server.exe because
sherpa-onnx is built against a newer ONNX Runtime than the DirectML one the recommendation
model uses, and Windows loads only one DLL of a given name per process.

Layout after this script:
  third_party/sherpa-onnx/include/sherpa-onnx/c-api/c-api.h
  third_party/sherpa-onnx/lib/sherpa-onnx-c-api.lib
  third_party/sherpa-onnx/bin/sherpa-onnx-c-api.dll, onnxruntime.dll, onnxruntime_providers_shared.dll

Set SHERPA_ONNX_URL_PREFIX to a mirror base (e.g. https://hk.gh-proxy.org/https://github.com)
when GitHub is slow; the release path is appended to it.
"""
import hashlib
import io
import os
import sys
import tarfile
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEST = os.path.join(ROOT, "third_party", "sherpa-onnx")
VERSION = "2.0.1"
BASE = os.environ.get("SHERPA_ONNX_URL_PREFIX", "https://github.com").rstrip("/")
URL = f"{BASE}/k2-fsa/sherpa-onnx/releases/download/v{VERSION}"

LIB_ASSET = f"sherpa-onnx-v{VERSION}-win-x64-shared-MD-Release-lib.tar.bz2"
RUN_ASSET = f"sherpa-onnx-v{VERSION}-win-x64-shared-MD-Release.tar.bz2"
LIB_SHA = "c3c78220db8bebf97d5f932496165bb3897b163410427d7c021013439c80f183"
RUN_SHA = "ce987d5bfca42c74b54f7b3bbaaa8c1febe3c5cd0fea8f94be80ea30e72f1f61"


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def download(url, want):
    print(f"downloading {url}")
    with urllib.request.urlopen(url, timeout=1200) as r:
        data = r.read()
    if sha256(data) != want:
        raise SystemExit(f"checksum mismatch for {url}")
    return data


def main():
    lib = download(f"{URL}/{LIB_ASSET}", LIB_SHA)
    run = download(f"{URL}/{RUN_ASSET}", RUN_SHA)

    os.makedirs(os.path.join(DEST, "include"), exist_ok=True)
    os.makedirs(os.path.join(DEST, "lib"), exist_ok=True)
    os.makedirs(os.path.join(DEST, "bin"), exist_ok=True)

    # Headers and the runtime DLLs from the release package; the import library from -lib.
    with tarfile.open(fileobj=io.BytesIO(run), mode="r:bz2") as tar:
        for member in tar.getmembers():
            name = member.name
            if "/include/" in name and member.isfile():
                rel = name.split("/include/", 1)[1]
                target = os.path.join(DEST, "include", rel)
                os.makedirs(os.path.dirname(target), exist_ok=True)
                with open(target, "wb") as f:
                    f.write(tar.extractfile(member).read())
            elif "/bin/" in name and member.isfile() and member.name.endswith(".dll"):
                base = os.path.basename(name)
                if base in ("sherpa-onnx-c-api.dll", "onnxruntime.dll",
                            "onnxruntime_providers_shared.dll"):
                    with open(os.path.join(DEST, "bin", base), "wb") as f:
                        f.write(tar.extractfile(member).read())

    with tarfile.open(fileobj=io.BytesIO(lib), mode="r:bz2") as tar:
        for member in tar.getmembers():
            base = os.path.basename(member.name)
            if not member.isfile():
                continue
            if base == "sherpa-onnx-c-api.lib":
                with open(os.path.join(DEST, "lib", base), "wb") as f:
                    f.write(tar.extractfile(member).read())
            elif base == "sherpa-onnx-c-api.dll":
                with open(os.path.join(DEST, "bin", base), "wb") as f:
                    f.write(tar.extractfile(member).read())

    print(f"sherpa-onnx {VERSION} -> {DEST}")


if __name__ == "__main__":
    main()
