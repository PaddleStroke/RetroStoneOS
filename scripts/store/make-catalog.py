#!/usr/bin/env python3
"""Build and sign the store catalog (docs/store-design.md).

  make-catalog.py --entries store/entries --base-url https://host -o out/ [--key store.key] [--mkupdate rsos-mkupdate]

Every entries/*.json describes one game: the catalog fields (id, name, system, version, author, license,
source, desc, kind, file | launcher, arch, cover) plus "payload": the local file to publish
(a ROM, or a ustar archive for kind tar/native). size, sha256, unpacked and url are filled in here.
Output: out/catalog.json, out/catalog.json.sig (with --key), and the payload files copied to out/files/.
An entry without a licence or a source URL is refused: the store only carries redistributable content.
"""
import argparse, hashlib, json, os, shutil, subprocess, sys, tarfile

REQUIRED = ["id", "name", "system", "version", "author", "license", "source", "desc", "kind", "payload"]


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def unpacked_size(path):
    """Sum of the file sizes of a tar (plain, or .tar.zst with Python 3.14's tarfile)."""
    total = 0
    with tarfile.open(path) as t:
        for m in t:
            if not (m.isfile() or m.isdir()):
                raise SystemExit(f"{path}: {m.name} is not a plain file or folder")
            if m.name.startswith("/") or ".." in m.name.split("/"):
                raise SystemExit(f"{path}: unsafe name {m.name}")
            if m.isfile():
                total += m.size
    return total


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--entries", required=True)
    ap.add_argument("--base-url", required=True)
    ap.add_argument("-o", "--out", required=True)
    ap.add_argument("--key")
    ap.add_argument("--key-env")
    ap.add_argument("--mkupdate", default="rsos-mkupdate")
    a = ap.parse_args()

    os.makedirs(os.path.join(a.out, "files"), exist_ok=True)
    entries, ids = [], set()
    for fn in sorted(os.listdir(a.entries)):
        if not fn.endswith(".json"):
            continue
        d = json.load(open(os.path.join(a.entries, fn), encoding="utf-8"))
        for k in REQUIRED:
            if not d.get(k):
                raise SystemExit(f"{fn}: missing {k}")
        if d["id"] in ids:
            raise SystemExit(f"{fn}: duplicate id {d['id']}")
        ids.add(d["id"])
        if d["kind"] not in ("rom", "tar", "native", "port"):
            raise SystemExit(f"{fn}: bad kind")
        if d["kind"] == "rom" and not d.get("file"):
            raise SystemExit(f"{fn}: a rom needs file")
        if d["kind"] == "native" and not (d.get("launcher") and d.get("arch")):
            raise SystemExit(f"{fn}: native needs launcher and arch")
        if d["kind"] == "port" and not (d.get("arch") and d.get("rom") and d["rom"].get("sha1") and d["rom"].get("mode")):
            raise SystemExit(f"{fn}: a port needs arch and a rom block (sha1, mode)")
        src = os.path.join(os.path.dirname(os.path.abspath(os.path.join(a.entries, fn))), d.pop("payload"))
        base = os.path.basename(src)
        shutil.copyfile(src, os.path.join(a.out, "files", base))
        d["size"] = os.path.getsize(src)
        d["sha256"] = sha256_file(src)
        d["unpacked"] = unpacked_size(src) if d["kind"] in ("tar", "native", "port") else d["size"]
        d["url"] = a.base_url.rstrip("/") + "/files/" + base
        entries.append(d)

    cat = {"format": 1, "entries": entries}
    path = os.path.join(a.out, "catalog.json")
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        json.dump(cat, f, ensure_ascii=False, indent=1, sort_keys=True)
        f.write("\n")
    print(f"{path}: {len(entries)} entries")
    if a.key or a.key_env:
        cmd = [a.mkupdate, "signfile"]
        cmd += ["-s", a.key] if a.key else ["--key-env", a.key_env]
        cmd += ["-o", path + ".sig", path]
        subprocess.check_call(cmd)


if __name__ == "__main__":
    sys.exit(main())
