# 2026-10-26: compare two -framedump directories frame by frame.
#   python -I tools/compare_dumps.py <dirA> <dirB> [--json out.json]
# For every fNNNNNN.raw/.pal in either directory: SHA-256 of each side, equal or not, and for a
# differing .raw the number of differing pixels and the first differing (x, y). The tree hash
# (SHA-256 over the sorted "name sha" lines of all .raw/.pal) identifies a whole run in one line.
# Exit code: 0 = identical (same file set, every file equal), 1 = any difference.
import hashlib
import json
import os
import sys


def sha(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def read_dims(d):
    w = h = None
    p = os.path.join(d, "dump.txt")
    if os.path.exists(p):
        for line in open(p):
            k, _, v = line.strip().partition(" ")
            if k == "width":
                w = int(v)
            elif k == "height":
                h = int(v)
    return w, h


def files(d):
    return sorted(f for f in os.listdir(d) if f.endswith((".raw", ".pal")))


def tree_hash(d, names, hashes):
    h = hashlib.sha256()
    for n in names:
        h.update(("%s %s\n" % (n, hashes[n])).encode())
    return h.hexdigest()


def main():
    a, b = sys.argv[1], sys.argv[2]
    out_json = sys.argv[sys.argv.index("--json") + 1] if "--json" in sys.argv else None
    fa, fb = files(a), files(b)
    ha = {n: sha(os.path.join(a, n)) for n in fa}
    hb = {n: sha(os.path.join(b, n)) for n in fb}
    w, h = read_dims(a)
    only_a = sorted(set(fa) - set(fb))
    only_b = sorted(set(fb) - set(fa))
    common = sorted(set(fa) & set(fb))
    diffs = []
    for n in common:
        if ha[n] == hb[n]:
            continue
        entry = {"file": n}
        if n.endswith(".raw") and w:
            pa = open(os.path.join(a, n), "rb").read()
            pb = open(os.path.join(b, n), "rb").read()
            idx = [i for i in range(min(len(pa), len(pb))) if pa[i] != pb[i]]
            entry["pixels_differing"] = len(idx)
            if idx:
                entry["first_xy"] = [idx[0] % w, idx[0] // w]
        diffs.append(entry)
    res = {
        "a": a, "b": b,
        "files_a": len(fa), "files_b": len(fb),
        "tree_sha256_a": tree_hash(a, fa, ha), "tree_sha256_b": tree_hash(b, fb, hb),
        "only_in_a": only_a, "only_in_b": only_b,
        "differing": len(diffs), "first_differences": diffs[:10],
        "identical": not only_a and not only_b and not diffs,
    }
    print("compare %s vs %s" % (a, b))
    print("  files %d / %d, differing %d, only-in-a %d, only-in-b %d" % (len(fa), len(fb), len(diffs), len(only_a), len(only_b)))
    print("  tree sha256 A %s" % res["tree_sha256_a"])
    print("  tree sha256 B %s" % res["tree_sha256_b"])
    for e in diffs[:5]:
        print("  diff %s" % e)
    print("  IDENTICAL" if res["identical"] else "  DIFFERENT")
    if out_json:
        with open(out_json, "w") as f:
            json.dump(res, f, indent=1)
    sys.exit(0 if res["identical"] else 1)


if __name__ == "__main__":
    main()
