#!/usr/bin/env python3
"""Tests for ue4_dump.py: builds synthetic UE4DMP01 dumps holding a fake class graph and checks the
class layouts read back. Exit code 0 = pass."""
import json
import os
import struct
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True  # importing ue4_dump must not leave a __pycache__ in tools/
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ue4_dump as U  # noqa: E402

FAILURES = []


def check(cond, what):
    if not cond:
        FAILURES.append(what)
        print("FAIL:", what)


def raises(fn, what):
    try:
        fn()
    except U.DumpError:
        return
    except Exception as e:  # wrong exception type is a failure too
        FAILURES.append("%s (raised %r)" % (what, e))
        print("FAIL:", what, repr(e))
        return
    FAILURES.append(what)
    print("FAIL:", what)


# ---- fake game build ------------------------------------------------------------------------------
PROFILE = U.Profile(
    uobject_class=0x08, uobject_name=0x18, ufield_next=0x28, ustruct_super=0x30, ustruct_children=0x38,
    ustruct_size=0x40, uproperty_offset=0x44, uproperty_size=0x48, uproperty_flags=0x50,
    uproperty_rep_index=0x58, ufunction_flags=0x60, gnames=0x20000000, name_chunk_size=4,
    name_string_offset=0x10)
OBJ = 0x80
HEAP = 0x10000000       # objects
NAMES = 0x10005000      # a hole (0x10004000-0x10005000) separates it from the objects
GOBJECTS = 0x10003000
CPF_NET = 0x20


class Image:
    """Guest memory as {address: bytes}; objects and names are allocated from a bump pointer."""

    def __init__(self):
        self.mem = {}
        self.heap = HEAP
        self.names = {}
        self.name_ptr = NAMES

    def put(self, addr, data):
        for i, b in enumerate(data):
            self.mem[addr + i] = b

    def name(self, text):
        if text not in self.names:
            idx = len(self.names) + 1  # 0 is "None"
            entry = self.name_ptr
            self.put(entry + PROFILE.name_string_offset, text.encode() + b"\0")
            self.name_ptr += 0x40
            self.names[text] = (idx, entry)
        return self.names[text][0]

    def obj(self, name, cls=0, **fields):
        addr = self.heap
        self.heap += OBJ
        p = PROFILE
        vals = {"uobject_class": cls, "uobject_name": self.name(name)}
        vals.update(fields)
        fmt = {"uobject_class": "<Q", "uobject_name": "<I", "ufield_next": "<Q", "ustruct_super": "<Q",
               "ustruct_children": "<Q", "ustruct_size": "<I", "uproperty_offset": "<I",
               "uproperty_size": "<I", "uproperty_flags": "<Q", "uproperty_rep_index": "<H",
               "ufunction_flags": "<I"}
        for k, v in vals.items():
            self.put(addr + getattr(p, k), struct.pack(fmt[k], v))
        return addr

    def finish_names(self):
        n = len(self.names) + 1
        chunks = (n + PROFILE.name_chunk_size - 1) // PROFILE.name_chunk_size
        for c in range(chunks):
            chunk = NAMES + 0x1000 + c * 0x100
            self.put(PROFILE.gnames + c * 8, struct.pack("<Q", chunk))
            for j in range(PROFILE.name_chunk_size):
                idx = c * PROFILE.name_chunk_size + j
                for _, (i, entry) in self.names.items():
                    if i == idx:
                        self.put(chunk + j * 8, struct.pack("<Q", entry))


def link(img, members):
    """Chains the members through ufield_next and returns the head."""
    for a, b in zip(members, members[1:]):
        img.put(a + PROFILE.ufield_next, struct.pack("<Q", b))
    return members[0] if members else 0


def build_image():
    img = Image()
    meta = img.obj("Class")
    img.put(meta + PROFILE.uobject_class, struct.pack("<Q", meta))  # Class is its own class
    float_c = img.obj("FloatProperty", meta)
    int_c = img.obj("IntProperty", meta)
    func_c = img.obj("Function", meta)

    id_ = img.obj("Id", int_c, uproperty_offset=0x28, uproperty_size=4, uproperty_flags=CPF_NET,
                  uproperty_rep_index=0)
    actor = img.obj("Actor", meta, ustruct_children=link(img, [id_]), ustruct_size=0x100)

    health = img.obj("Health", float_c, uproperty_offset=0x120, uproperty_size=4,
                     uproperty_flags=CPF_NET | 0x100, uproperty_rep_index=1)
    local = img.obj("LocalOnly", float_c, uproperty_offset=0x124, uproperty_size=4,
                    uproperty_flags=0, uproperty_rep_index=0xFFFF)
    server = img.obj("ServerMove", func_c, ufunction_flags=0x40 | 0x200000 | 0x80)
    client = img.obj("ClientAck", func_c, ufunction_flags=0x40 | 0x1000000)
    multi = img.obj("Multi", func_c, ufunction_flags=0x40 | 0x4000)
    plain = img.obj("Plain", func_c, ufunction_flags=0x400)
    pawn = img.obj("FortPawn", meta, ustruct_super=actor, ustruct_size=0x130,
                   ustruct_children=link(img, [health, local, server, client, multi, plain]))
    img.finish_names()

    objs = [actor, 0, pawn, id_, health, meta]  # a null slot and non-class objects are skipped
    img.put(GOBJECTS, b"".join(struct.pack("<Q", o) for o in objs))
    return img, {"actor": actor, "pawn": pawn, "meta": meta, "count": len(objs)}


def write_dump(path, regions, magic=b"UE4DMP01", count=None, pad=0, trunc=0):
    """regions: [(base, bytes, prot)]."""
    blob = magic + struct.pack("<Q", len(regions) if count is None else count)
    for base, data, prot in regions:
        blob += struct.pack("<QQII", base, len(data), prot, pad) + data
    with open(path, "wb") as f:
        f.write(blob[:len(blob) - trunc] if trunc else blob)


def regions_of(img):
    """Whole pages: objects, GObjects, the name entries and chunks (past a hole), the GNames table."""
    spans = [(HEAP, 0x1000), (GOBJECTS, 0x1000), (NAMES, 0x2000), (PROFILE.gnames, 0x1000)]
    return [(base, bytes(img.mem.get(base + i, 0) for i in range(size)), 3) for base, size in spans]


# ---- tests ------------------------------------------------------------------------------------------
def test_format(tmp):
    path = os.path.join(tmp, "f.bin")
    a = bytes(range(256)) * 16          # 0x1000 bytes
    b = b"\xAA" * 0x1000
    write_dump(path, [(0x2000, b, 5), (0x1000, a, 3), (0x4000, b"\x55" * 16, 1)])  # unsorted on purpose
    with U.Dump(path) as d:
        check(d.declared_regions == 3, "declared region count read")
        check([r[0] for r in d.regions] == [0x1000, 0x2000, 0x4000], "regions sorted by base")
        check([r[2] for r in d.regions] == [3, 5, 1], "protection bits kept per region")
        check(d.read(0x1000, 4) == a[:4], "read inside a region")
        check(d.read(0x1FFE, 4) == a[-2:] + b[:2], "read across touching regions")
        check(d.u32(0x1000) == struct.unpack("<I", a[:4])[0], "u32")
        check(d.u64(0x1008) == struct.unpack("<Q", a[8:16])[0], "u64")
        raises(lambda: d.read(0x2FFE, 4), "read into the gap after a region fails")
        raises(lambda: d.read(0xFFF, 1), "read before the first region fails")
        raises(lambda: d.read(0x4000 + 15, 2), "read past the last region fails")
        hits = list(d.find(b"\x55\x55"))
        check(hits == [0x4000 + i for i in range(15)], "find yields every overlapping match")
        check(list(d.find(b"\xAA" * 4))[:2] == [0x2000, 0x2001], "find in a later region")

    write_dump(path, [(0x1000, a, 3)], count=0)  # the count is informational (a crashed dump has 0)
    with U.Dump(path) as d:
        check(len(d.regions) == 1 and d.read(0x1000, 1) == a[:1], "readers go on to EOF whatever the count says")

    write_dump(path, [])
    with U.Dump(path) as d:
        check(d.regions == [], "header-only dump has no regions")

    write_dump(path, [(0x1000, a, 3)], magic=b"UE4DMP02")
    raises(lambda: U.Dump(path), "wrong magic rejected")
    write_dump(path, [(0x1000, a, 3)], pad=1)
    raises(lambda: U.Dump(path), "nonzero padding rejected")
    write_dump(path, [(0x1000, a, 3)], trunc=10)
    raises(lambda: U.Dump(path), "truncated region data rejected")
    write_dump(path, [(0x1000, a, 3)], trunc=len(a) + 10)
    raises(lambda: U.Dump(path), "truncated region header rejected")
    open(path, "wb").write(b"UE4")
    raises(lambda: U.Dump(path), "file shorter than the header rejected")
    open(path, "wb").close()
    raises(lambda: U.Dump(path), "empty file rejected")


def test_layout(tmp, split):
    img, addr = build_image()
    path = os.path.join(tmp, "l%d.bin" % split)
    regions = regions_of(img)
    if split:  # same bytes, split into touching records with different protection
        out = []
        for base, data, prot in regions:
            if base == HEAP:  # the cut falls inside an object, through its super pointer
                out += [(base, data[:0x234], 1), (base + 0x234, data[0x234:], 3)]
            else:
                out.append((base, data, prot))
        regions = out
    write_dump(path, regions)

    with U.Dump(path) as d:
        p = PROFILE
        check(U.object_name(d, p, addr["pawn"]) == "FortPawn", "object name through GNames chunks")
        check(U.class_name(d, p, addr["pawn"]) == "Class", "class of a class is Class")

        actor = U.dump_class(d, p, addr["actor"])
        check(actor["name"] == "Actor" and actor["super"] is None and actor["size"] == 0x100, "Actor header")
        check([x["name"] for x in actor["properties"]] == ["Id"] and actor["functions"] == [], "Actor members")

        pawn = U.dump_class(d, p, addr["pawn"])
        check(pawn["super"] == "Actor" and pawn["size"] == 0x130, "FortPawn header")
        props = {x["name"]: x for x in pawn["properties"]}
        check(list(props) == ["Health", "LocalOnly"], "property order follows the child list")
        h = props["Health"]
        check((h["type"], h["offset"], h["size"], h["rep_index"]) == ("FloatProperty", 0x120, 4, 1),
              "Health layout")
        check(h["replicated"] and h["flags"] == CPF_NET | 0x100, "Health is replicated, flags kept whole")
        l = props["LocalOnly"]
        check(not l["replicated"] and l["rep_index"] == 0xFFFF and l["offset"] == 0x124, "LocalOnly layout")

        funcs = {x["name"]: x["rpc"] for x in pawn["functions"]}
        check(funcs == {"ServerMove": "server", "ClientAck": "client", "Multi": "multicast", "Plain": None},
              "RPC kinds from function flags")
        check(pawn["functions"][0]["flags"] == 0x40 | 0x200000 | 0x80, "function flags kept whole")

        chain = U.full_layout(d, p, addr["pawn"])
        check([c["name"] for c in chain] == ["Actor", "FortPawn"], "full layout is root first")
        check(U.full_layout(d, p, addr["actor"])[0]["name"] == "Actor", "root class layout")

        found = U.find_classes(d, p, GOBJECTS, addr["count"])
        check(found == [addr["actor"], addr["pawn"], addr["meta"]],
              "find_classes keeps classes, skips null slots and non-class objects")


def test_loops(tmp):
    img, addr = build_image()
    img.put(addr["actor"] + PROFILE.ustruct_super, struct.pack("<Q", addr["pawn"]))  # Actor <-> FortPawn
    path = os.path.join(tmp, "loop.bin")
    write_dump(path, regions_of(img))
    with U.Dump(path) as d:
        raises(lambda: U.full_layout(d, PROFILE, addr["pawn"]), "super-chain cycle detected")

    img, addr = build_image()
    first = struct.unpack("<Q", bytes(img.mem[addr["pawn"] + PROFILE.ustruct_children + i] for i in range(8)))[0]
    img.put(first + PROFILE.ufield_next, struct.pack("<Q", first))  # first member points at itself
    write_dump(path, regions_of(img))
    with U.Dump(path) as d:
        raises(lambda: U.dump_class(d, PROFILE, addr["pawn"]), "child-list cycle detected")


def test_missing_memory(tmp):
    img, addr = build_image()
    path = os.path.join(tmp, "gap.bin")
    regions = [r for r in regions_of(img) if not (NAMES <= r[0] < NAMES + 0x10000)]  # names never dumped
    write_dump(path, regions)
    with U.Dump(path) as d:
        raises(lambda: U.dump_class(d, PROFILE, addr["pawn"]), "unmapped name table fails loudly")


def test_cli(tmp):
    img, addr = build_image()
    dump = os.path.join(tmp, "cli.bin")
    write_dump(dump, regions_of(img))
    prof = os.path.join(tmp, "profile.json")
    raw = {k: hex(v) for k, v in vars(PROFILE).items()}
    with open(prof, "w") as f:
        json.dump(raw, f)
    out = subprocess.run([sys.executable, "-B", os.path.join(os.path.dirname(os.path.abspath(__file__)), "ue4_dump.py"),
                          dump, prof, hex(GOBJECTS), str(addr["count"])], capture_output=True, text=True)
    check(out.returncode == 0, "CLI exits 0 (%s)" % out.stderr.strip()[-200:])
    if out.returncode == 0:
        layouts = json.loads(out.stdout)
        names = [[c["name"] for c in chain] for chain in layouts]
        check(["Actor"] in names and ["Actor", "FortPawn"] in names, "CLI prints the class chains")

    bad = json.load(open(prof))
    del bad["gnames"]
    bad["extra"] = 1
    json.dump(bad, open(prof, "w"))
    raises(lambda: U.Profile.from_json(prof), "profile with missing/unknown keys rejected")
    out = subprocess.run([sys.executable, "-B", os.path.join(os.path.dirname(os.path.abspath(__file__)), "ue4_dump.py")],
                         capture_output=True, text=True)
    check(out.returncode == 2, "CLI usage error exits 2")


def main():
    with tempfile.TemporaryDirectory() as tmp:
        test_format(tmp)
        test_layout(tmp, split=0)
        test_layout(tmp, split=1)
        test_loops(tmp)
        test_missing_memory(tmp)
        test_cli(tmp)
    if FAILURES:
        print("%d check(s) failed" % len(FAILURES))
        return 1
    print("ue4_dump: all tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
