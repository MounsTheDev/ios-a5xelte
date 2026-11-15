#!/usr/bin/env python3
"""Walk a live /proc/device-tree dump and resolve phandles.

Read-only helper for the HEROLTE port: the Samsung device tree exposes
properties as binary files, and the pinctrl/gpio bindings are unreadable
without resolving phandles across the whole tree.
"""
import os
import struct
import sys

ROOT = sys.argv[1] if len(sys.argv) > 1 else "build/dt-live"


def read_prop(path):
    with open(path, "rb") as f:
        return f.read()


def walk(node):
    for entry in sorted(os.listdir(node)):
        p = os.path.join(node, entry)
        if os.path.isdir(p):
            yield from walk(p)
        # properties are files


def nodes(root):
    yield root
    for entry in sorted(os.listdir(root)):
        p = os.path.join(root, entry)
        if os.path.isdir(p):
            yield from nodes(p)


def props(node):
    out = {}
    for entry in sorted(os.listdir(node)):
        p = os.path.join(node, entry)
        if os.path.isfile(p):
            out[entry] = read_prop(p)
    return out


def u32s(blob):
    return list(struct.unpack(">%dI" % (len(blob) // 4), blob[: len(blob) // 4 * 4]))


def cstr(blob):
    return blob.split(b"\0")[0].decode("utf-8", "replace")


def main():
    phandles = {}
    all_nodes = list(nodes(ROOT))
    for n in all_nodes:
        p = props(n)
        for key in ("phandle", "linux,phandle"):
            if key in p and len(p[key]) >= 4:
                ph = struct.unpack(">I", p[key][:4])[0]
                phandles[ph] = os.path.relpath(n, ROOT)
    print("== phandle map (%d)" % len(phandles))
    for ph in sorted(phandles):
        print("  %#06x -> %s" % (ph, phandles[ph]))

    targets = [
        "i2c@20",
        "i2c@20/touchkey@20",
        "i2c@20/touchkey@40",
    ]
    for t in targets:
        n = os.path.join(ROOT, t)
        if not os.path.isdir(n):
            continue
        print("\n== %s" % t)
        for k, v in props(n).items():
            if k in ("name", "compatible", "status"):
                print("  %-28s %r" % (k, cstr(v)))
            elif len(v) % 4 == 0 and len(v) <= 64:
                vals = u32s(v)
                extra = ""
                if k.endswith("gpio") or k == "gpios" or k.startswith("pinctrl-") or k == "interrupt-parent":
                    extra = "  -> " + ", ".join(
                        phandles.get(x, "?%#x" % x) for x in vals[::3] if True
                    )
                print("  %-28s %s%s" % (k, " ".join("%#x" % x for x in vals), extra))
            else:
                print("  %-28s <%d bytes> %r" % (k, len(v), v[:32]))

    print("\n== gpio / interrupt controllers")
    for n in all_nodes:
        p = props(n)
        if "gpio-controller" in p or "interrupt-controller" in p:
            rel = os.path.relpath(n, ROOT)
            reg = u32s(p["reg"]) if "reg" in p else []
            print("  %-45s reg=%s gpio_cells=%s irq_cells=%s%s%s" % (
                rel,
                " ".join("%#x" % x for x in reg),
                u32s(p["#gpio-cells"])[0] if "#gpio-cells" in p else "-",
                u32s(p["#interrupt-cells"])[0] if "#interrupt-cells" in p else "-",
                " GPIO" if "gpio-controller" in p else "",
                " IRQ" if "interrupt-controller" in p else "",
            ))
            if "phandle" in p:
                print("       phandle=%#x" % struct.unpack(">I", p["phandle"][:4])[0])


if __name__ == "__main__":
    main()
