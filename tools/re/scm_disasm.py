"""Minimal GTA SA SCM disassembler (main.scm / script.img scripts) using the Sanny Builder opcode
database (re/sa_commands.json).

usage:
  python tools/re/scm_disasm.py main.scm --start 0x1234 --count 40     # disassemble from an offset
  python tools/re/scm_disasm.py main.scm --find 0417                     # offsets of an opcode in the main part
"""
import argparse
import json
import os
import struct

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def load_commands():
    db = json.load(open(os.path.join(ROOT, "re", "sa_commands.json"), encoding="utf-8"))
    cmds = {}
    for ext in db["extensions"]:
        for c in ext["commands"]:
            cmds[int(c["id"], 16)] = c
    return cmds


# data type -> (payload size, formatter)
def read_arg(data, pos):
    t = data[pos]
    pos += 1
    if t == 0x00:
        return None, pos
    if t == 0x01:
        return str(struct.unpack_from("<i", data, pos)[0]), pos + 4
    if t in (0x02, 0x0A, 0x10):
        return f"${struct.unpack_from('<H', data, pos)[0]}", pos + 2
    if t in (0x03, 0x0B, 0x11):
        return f"{struct.unpack_from('<H', data, pos)[0]}@", pos + 2
    if t == 0x04:
        return str(struct.unpack_from("<b", data, pos)[0]), pos + 1
    if t == 0x05:
        return str(struct.unpack_from("<h", data, pos)[0]), pos + 2
    if t == 0x06:
        return f"{struct.unpack_from('<f', data, pos)[0]:.3f}", pos + 4
    if t in (0x07, 0x08, 0x0C, 0x0D, 0x12, 0x13):
        off, idx, size, flags = struct.unpack_from("<HHbb", data, pos)
        kind = "$" if t in (0x07, 0x0C, 0x12) else ""
        return f"{kind}{off}({idx}@,{size})", pos + 6
    if t == 0x09:
        return "'" + data[pos:pos + 8].split(b"\0")[0].decode("latin-1") + "'", pos + 8
    if t == 0x0E:
        n = data[pos]
        return '"' + data[pos + 1:pos + 1 + n].decode("latin-1") + '"', pos + 1 + n
    if t == 0x0F:
        return "'" + data[pos:pos + 16].split(b"\0")[0].decode("latin-1") + "'", pos + 16
    # legacy: 8-byte string without a type byte
    return "'" + data[pos - 1:pos + 7].split(b"\0")[0].decode("latin-1") + "'", pos + 7


def disasm_one(data, pos, cmds):
    raw = struct.unpack_from("<H", data, pos)[0]
    op, neg = raw & 0x7FFF, raw & 0x8000
    c = cmds.get(op)
    name = c["name"] if c else f"OP_{op:04X}"
    p = pos + 2
    args = []
    n = c.get("num_params", 0) if c else 0
    variadic = c and any(i.get("type") == "arguments" for i in c.get("input", [])) or (c and n == -1)
    count = n if n >= 0 else 99
    i = 0
    ended = False  # the 00 terminator already read (START_NEW_SCRIPT with no arguments for the script)
    while i < count:
        a, p = read_arg(data, p)
        if a is None:  # end of variadic arguments
            ended = True
            break
        args.append(a)
        i += 1
        if variadic and i >= count:
            break
    if variadic and n >= 0 and not ended:  # remaining variadic arguments until the 00 terminator
        while p < len(data) and data[p] != 0:
            a, p = read_arg(data, p)
            args.append(a)
        p += 1
    return f"{pos:08X}: {'NOT ' if neg else ''}{name} {' '.join(args)}", p


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("scm")
    ap.add_argument("--start", type=lambda s: int(s, 0))
    ap.add_argument("--count", type=int, default=30)
    ap.add_argument("--find")
    a = ap.parse_args()
    data = open(a.scm, "rb").read()
    cmds = load_commands()
    if a.find:
        op = int(a.find, 16)
        lo = struct.pack("<H", op)
        hits = [i for i in range(len(data) - 1) if data[i:i + 2] == lo]
        print("\n".join(f"{h:#x}" for h in hits[:200]), f"\n{len(hits)} raw matches (not all are instructions)")
        return
    pos = a.start
    for _ in range(a.count):
        line, pos = disasm_one(data, pos, cmds)
        print(line)


if __name__ == "__main__":
    main()
