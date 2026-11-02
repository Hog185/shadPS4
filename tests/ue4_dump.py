#!/usr/bin/env python3
"""Reads ue4_memdump dumps (core/ue4_memdump.cpp) and extracts UE4 class layouts.

Dump format (little endian): "UE4DMP01", u64 region count (informational), then per region
u64 base, u64 size, u32 prot (bit0 r, bit1 w, bit2 x), u32 0, <size bytes>.
Object offsets differ per game build, so the walker takes them from a Profile.
"""
import bisect
import json
import mmap
import struct
import sys
from dataclasses import dataclass, fields

MAGIC = b"UE4DMP01"
CPF_NET = 0x20
FUNC_NET = 0x40
FUNC_NET_MULTICAST = 0x4000
FUNC_NET_SERVER = 0x200000
FUNC_NET_CLIENT = 0x1000000
MAX_WALK = 100000


class DumpError(Exception):
    pass


class Dump:
    def __init__(self, path):
        self._f = open(path, "rb")
        try:
            self._mm = mmap.mmap(self._f.fileno(), 0, access=mmap.ACCESS_READ)
        except ValueError:  # empty file
            self._f.close()
            raise DumpError("empty file")
        self.regions = []  # (base, size, prot, file offset), sorted by base
        try:
            self._parse()
        except Exception:
            self.close()
            raise
        self._bases = [r[0] for r in self.regions]

    def close(self):
        self._mm.close()
        self._f.close()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    def _parse(self):
        mm, end = self._mm, len(self._mm)
        if end < 16 or mm[:8] != MAGIC:
            raise DumpError("not a UE4DMP01 dump")
        self.declared_regions = struct.unpack_from("<Q", mm, 8)[0]
        pos = 16
        while pos < end:
            if pos + 24 > end:
                raise DumpError("truncated region header at file offset %d" % pos)
            base, size, prot, pad = struct.unpack_from("<QQII", mm, pos)
            pos += 24
            if pad != 0:
                raise DumpError("bad region header at file offset %d" % (pos - 24))
            if pos + size > end:
                raise DumpError("truncated region 0x%x (%d of %d bytes)" % (base, end - pos, size))
            self.regions.append((base, size, prot, pos))
            pos += size
        self.regions.sort()

    def _region(self, addr):
        i = bisect.bisect_right(self._bases, addr) - 1
        if i >= 0:
            base, size = self.regions[i][:2]
            if addr < base + size:
                return self.regions[i]
        raise DumpError("address 0x%x is not in the dump" % addr)

    def read(self, addr, n):
        out = bytearray()
        while len(out) < n:  # regions that touch are read as one
            base, size, _, off = self._region(addr + len(out))
            start = addr + len(out) - base
            take = min(n - len(out), size - start)
            out += self._mm[off + start:off + start + take]
        return bytes(out)

    def u16(self, addr):
        return struct.unpack("<H", self.read(addr, 2))[0]

    def u32(self, addr):
        return struct.unpack("<I", self.read(addr, 4))[0]

    def u64(self, addr):
        return struct.unpack("<Q", self.read(addr, 8))[0]

    def cstr(self, addr, limit=256):
        out = bytearray()
        while len(out) < limit:
            b = self.read(addr + len(out), 1)
            if b == b"\0":
                break
            out += b
        return out.decode("ascii", "replace")

    def find(self, pattern):
        """Yields every guest address where pattern occurs (matches never span regions)."""
        for base, size, _, off in self.regions:
            pos = self._mm.find(pattern, off, off + size)
            while pos != -1:
                yield base + pos - off
                pos = self._mm.find(pattern, pos + 1, off + size)


@dataclass
class Profile:
    """Offsets of one game build. name_* describe GNames: a table of chunk pointers, each chunk an
    array of FNameEntry pointers, the entry's string at name_string_offset."""
    uobject_class: int
    uobject_name: int
    ufield_next: int
    ustruct_super: int
    ustruct_children: int
    ustruct_size: int
    uproperty_offset: int
    uproperty_size: int
    uproperty_flags: int
    uproperty_rep_index: int
    ufunction_flags: int
    gnames: int
    name_chunk_size: int
    name_string_offset: int

    @classmethod
    def from_json(cls, path):
        with open(path) as f:
            raw = json.load(f)
        names = {f.name for f in fields(cls)}
        if set(raw) != names:
            raise DumpError("profile keys differ: missing %s, unknown %s" % (
                sorted(names - set(raw)), sorted(set(raw) - names)))
        return cls(**{k: int(v, 0) if isinstance(v, str) else v for k, v in raw.items()})


def fname(d, p, index):
    chunk = d.u64(p.gnames + (index // p.name_chunk_size) * 8)
    entry = d.u64(chunk + (index % p.name_chunk_size) * 8)
    return d.cstr(entry + p.name_string_offset)


def object_name(d, p, obj):
    return fname(d, p, d.u32(obj + p.uobject_name))


def class_name(d, p, obj):
    return object_name(d, p, d.u64(obj + p.uobject_class))


def rpc_kind(flags):
    if not flags & FUNC_NET:
        return None
    if flags & FUNC_NET_SERVER:
        return "server"
    if flags & FUNC_NET_CLIENT:
        return "client"
    if flags & FUNC_NET_MULTICAST:
        return "multicast"
    return "net"


def struct_members(d, p, struct_addr):
    """Own properties and functions of one UStruct, in declaration-list order."""
    props, funcs = [], []
    seen = set()
    child = d.u64(struct_addr + p.ustruct_children)
    while child:
        if child in seen or len(seen) > MAX_WALK:
            raise DumpError("child list of 0x%x loops" % struct_addr)
        seen.add(child)
        kind = class_name(d, p, child)
        if kind == "Function":
            flags = d.u32(child + p.ufunction_flags)
            funcs.append({"name": object_name(d, p, child), "flags": flags, "rpc": rpc_kind(flags)})
        else:
            flags = d.u64(child + p.uproperty_flags)
            props.append({
                "name": object_name(d, p, child), "type": kind,
                "offset": d.u32(child + p.uproperty_offset), "size": d.u32(child + p.uproperty_size),
                "flags": flags, "replicated": bool(flags & CPF_NET),
                "rep_index": d.u16(child + p.uproperty_rep_index),
            })
        child = d.u64(child + p.ufield_next)
    return props, funcs


def dump_class(d, p, class_addr):
    props, funcs = struct_members(d, p, class_addr)
    sup = d.u64(class_addr + p.ustruct_super)
    return {"name": object_name(d, p, class_addr), "super": object_name(d, p, sup) if sup else None,
            "size": d.u32(class_addr + p.ustruct_size), "properties": props, "functions": funcs}


def full_layout(d, p, class_addr):
    """The class and its ancestors, root first."""
    chain, addr = [], class_addr
    while addr:
        if len(chain) > 64:
            raise DumpError("super chain of 0x%x loops" % class_addr)
        chain.append(dump_class(d, p, addr))
        addr = d.u64(addr + p.ustruct_super)
    return chain[::-1]


def find_classes(d, p, gobjects, count):
    """Addresses of every UClass in a flat GObjects array of `count` object pointers."""
    out = []
    for i in range(count):
        obj = d.u64(gobjects + i * 8)
        if obj and class_name(d, p, obj) == "Class":
            out.append(obj)
    return out


def main(argv):
    if len(argv) != 5:
        print("usage: ue4_dump.py DUMP PROFILE.json GOBJECTS_ADDR COUNT", file=sys.stderr)
        return 2
    p = Profile.from_json(argv[2])
    with Dump(argv[1]) as d:
        classes = find_classes(d, p, int(argv[3], 0), int(argv[4], 0))
        result = [full_layout(d, p, c) for c in classes]
    json.dump(result, sys.stdout, indent=1)
    print()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
