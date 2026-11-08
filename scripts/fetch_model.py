"""Downloads the Laya model used by 知语输入法 into models/laya/, and the word n-gram next to it
into models/ngram/.

  python scripts/fetch_model.py            # download the release assets and verify them
  python scripts/fetch_model.py --pack     # maintainers: zip models/laya/ and models/ngram/

The model (laya.int8g.onnx and laya.one.fused.int8g.onnx, ~75 MB each) and the n-gram (zh.ngram
~79 MB) are too large for git; each is published as a GitHub release asset of this repository
(model-ml-flash-1103, the same files 知意输入法 ships from 1.6.4; ngram-zh-1017). The training code
and data are not published.
"""
import argparse
import hashlib
import io
import os
import sys
import urllib.request
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEST = os.path.join(ROOT, "models", "laya")
# laya.int8g.onnx: the whole graph (on a graphics card); laya.one.fused.int8g.onnx: the one graph
# with the context's KV cache, run on the CPU (engine/src/laya/reranker.h).
FILES = ["laya.int8g.onnx", "laya.one.fused.int8g.onnx", "tokenizer.json", "rl_agent_config.json",
         "unified_vocab.json"]
# The decoder graphs of the 1.5.4-1.5.8 model: removed from models/laya (the packager ships them
# when present).
OLD_FILES = ["laya_decoder.int8.onnx", "laya_decoder_memory.int8.onnx",
             "laya_decoder_step.int8.onnx"]
ASSET = "laya-ml-flash-1103-int8g.zip"
URL = f"https://github.com/w3c0929/zhiyi_ime/releases/download/model-ml-flash-1103/{ASSET}"
# SHA-256 of the graphs: 知语 ML-Flash-1103 (upstream 知意 1.6.4). ML-pumpkin-1026 cut to 8 layers
# with a prefix mask (the leading block, the context, attends only to itself, so its keys and values
# are cached per context) and one head shared by word choice and composition, distilled from
# Qwen3.5-9B LoRA teachers and trained further on rows typed without context. int8, 126963
# embedding rows, the tokenizer of 1.4V2-Long-L18. laya.one.fused.int8g.onnx is the one graph with
# ONNX Runtime's fused attention, run on the CPU with laya.context_cache.
ONNX_SHA256 = "6396bffede92d3120c25d5ded1c96bd65e18081c17adf0d81ddd34c41afe3db7"
ONE_SHA256 = "d08f7a0e7036f4617aa20db5ec8c4019b53605382d37c46050515e572edaeacc"

# The word n-gram scored next to the word choice (laya.ngram, engine/src/laya/ngram.h), ng1017:
# Youmo IME's pinyin language model (LGPL-2.1-or-later, on libime's) with science text mixed in
# (CSL abstracts and science pages of SkyPile, C4, FineWeb-2), pruned by relative entropy to 4M
# bigrams and 4M trigrams. We re-publish the same file as the release ngram-zh-1017.
NGRAM_DEST = os.path.join(ROOT, "models", "ngram")
NGRAM_FILE = "zh.ngram"
NGRAM_ASSET = "zhiyi-ngram-zh-1017.zip"
NGRAM_URL = f"https://github.com/w3c0929/zhiyi_ime/releases/download/ngram-zh-1017/{NGRAM_ASSET}"
NGRAM_SHA256 = "0a9e5c151d7dc90665358b3f6ab10436520457a26e9cfe1fa2054d6c046f72cd"


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def pack():
    out = os.path.join(ROOT, "build", ASSET)
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        for f in FILES:
            z.write(os.path.join(DEST, f), f)
    print(f"{out}\nlaya.int8g.onnx sha256 {sha256(os.path.join(DEST, 'laya.int8g.onnx'))}"
          f"\nlaya.one.fused.int8g.onnx sha256 "
          f"{sha256(os.path.join(DEST, 'laya.one.fused.int8g.onnx'))}")
    out = os.path.join(ROOT, "build", NGRAM_ASSET)
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        z.write(os.path.join(NGRAM_DEST, NGRAM_FILE), NGRAM_FILE)
    print(f"{out}\n{NGRAM_FILE} sha256 {sha256(os.path.join(NGRAM_DEST, NGRAM_FILE))}")


def matches():
    if not all(os.path.exists(os.path.join(DEST, f)) for f in FILES):
        return False
    return (sha256(os.path.join(DEST, "laya.int8g.onnx")) == ONNX_SHA256 and
            sha256(os.path.join(DEST, "laya.one.fused.int8g.onnx")) == ONE_SHA256)


def fetch_ngram():
    path = os.path.join(NGRAM_DEST, NGRAM_FILE)
    if os.path.exists(path) and sha256(path) == NGRAM_SHA256:
        print(f"n-gram already in {NGRAM_DEST}")
        return 0
    print(f"downloading {NGRAM_URL}")
    with urllib.request.urlopen(NGRAM_URL, timeout=600) as r:
        data = r.read()
    os.makedirs(NGRAM_DEST, exist_ok=True)
    with zipfile.ZipFile(io.BytesIO(data)) as z:
        with open(path, "wb") as dst:
            dst.write(z.read(NGRAM_FILE))
    if sha256(path) != NGRAM_SHA256:
        print("checksum mismatch for the n-gram", file=sys.stderr)
        return 1
    print(f"n-gram -> {NGRAM_DEST}")
    return 0


def fetch():
    for f in OLD_FILES:
        if os.path.exists(os.path.join(DEST, f)):
            os.remove(os.path.join(DEST, f))
    if fetch_ngram() != 0:
        return 1
    if matches():
        print(f"model already in {DEST}")
        return 0
    print(f"downloading {URL}")
    with urllib.request.urlopen(URL, timeout=600) as r:
        data = r.read()
    os.makedirs(DEST, exist_ok=True)
    with zipfile.ZipFile(io.BytesIO(data)) as z:
        for f in FILES:
            with open(os.path.join(DEST, f), "wb") as dst:
                dst.write(z.read(f))
    if not matches():
        print("checksum mismatch for the model files", file=sys.stderr)
        return 1
    print(f"model -> {DEST}")
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--pack", action="store_true")
    a = ap.parse_args()
    if a.pack:
        pack()
        return 0
    return fetch()


if __name__ == "__main__":
    sys.exit(main())
