#!/usr/bin/env python3
"""Read/write Mass Effect (Xbox 360) Coalesced.ini: big-endian u32 string count
(two strings per file), then for each file a u32-length-prefixed name and a
u32-length-prefixed contents.

  coalesced.py check  <Coalesced.ini>                 round-trip + list files
  coalesced.py set-map <in> <out> <MapName>           set [URL] LocalMap (startup map)
  coalesced.py no-startup-movies <in> <out>           drop StartupMovie= lines (logos; debug runs)
  coalesced.py set <in> <out> <file-substr> <Key> <Value>  set Key=Value in the matching ini (experiments)
"""
import struct, sys

def parse(data):
    off = 0
    (count,) = struct.unpack_from(">I", data, off); off += 4
    entries = []
    if count % 2:
        raise ValueError(f"odd string count {count}")
    for _ in range(count // 2):
        (n,) = struct.unpack_from(">I", data, off); off += 4
        name = data[off:off + n]; off += n
        (m,) = struct.unpack_from(">I", data, off); off += 4
        body = data[off:off + m]; off += m
        entries.append([name, body])
    if off != len(data):
        raise ValueError(f"trailing bytes: parsed {off} of {len(data)}")
    return entries

def build(entries):
    out = [struct.pack(">I", 2 * len(entries))]
    for name, body in entries:
        out += [struct.pack(">I", len(name)), name, struct.pack(">I", len(body)), body]
    return b"".join(out)

def main():
    cmd = sys.argv[1]
    data = open(sys.argv[2], "rb").read()
    entries = parse(data)
    if cmd == "check":
        assert build(entries) == data, "round-trip mismatch"
        for name, body in entries:
            print(f"{len(body):7d}  {name.rstrip(b'\\0').decode()}")
        print("round-trip OK")
    elif cmd == "set-map":
        new_map = sys.argv[4].encode()
        hits = 0
        for e in entries:
            if b"LocalMap=" in e[1]:
                lines = e[1].split(b"\r\n")
                for i, l in enumerate(lines):
                    if l.startswith(b"LocalMap="):
                        print(f"{e[0].rstrip(bytes(1)).decode()}: {l.decode()} -> LocalMap={new_map.decode()}")
                        lines[i] = b"LocalMap=" + new_map; hits += 1
                e[1] = b"\r\n".join(lines)
        if hits != 1:
            raise SystemExit(f"expected exactly one LocalMap line, found {hits}")
        open(sys.argv[3], "wb").write(build(entries))
    elif cmd == "no-startup-movies":
        hits = 0
        for e in entries:
            lines = e[1].split(b"\r\n")
            kept = [l for l in lines if not l.startswith(b"StartupMovie=")]
            hits += len(lines) - len(kept)
            e[1] = b"\r\n".join(kept)
        print(f"removed {hits} StartupMovie lines")
        open(sys.argv[3], "wb").write(build(entries))
    elif cmd == "set":
        set_key(entries, sys.argv[4], sys.argv[5], sys.argv[6])
        open(sys.argv[3], "wb").write(build(entries))

def set_key(entries, file_sub, key, value):
    hits = 0
    for e in entries:
        if file_sub.encode() not in e[0]:
            continue
        lines = e[1].split(b"\r\n")
        for i, l in enumerate(lines):
            if l.split(b"=", 1)[0] == key.encode():
                print(f"{e[0].rstrip(bytes(1)).decode()}: {l.decode()} -> {key}={value}")
                lines[i] = (key + "=" + value).encode(); hits += 1
        e[1] = b"\r\n".join(lines)
    if hits != 1:
        raise SystemExit(f"expected exactly one {key} line in *{file_sub}*, found {hits}")

if __name__ == "__main__":
    main()
