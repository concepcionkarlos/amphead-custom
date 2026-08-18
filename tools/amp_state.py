#!/usr/bin/env python
"""
Build a VST3 raw_state blob for the plugin, so a render can use the state the
player actually has - INCLUDING the loaded cabinet IRs.

This exists because the IR stage was the one part of the signal path no test had
ever touched: amp_test.py hard-codes bypass_ir=True, and pedalboard can set
parameters but cannot click "load IR" in the editor. The IR file paths, however,
live in the saved state, and pedalboard exposes raw_state - so the state can be
rewritten with paths that exist and handed back to the plugin.

Format, working outwards:
  inner  : <?xml ...?><APVTS .../><IRFILES a=".." b=".." dir=".."/>
  wrapped: b"VC2!" + uint32 little-endian length + that XML   (JUCE MemoryBlock)
  encoded: "<decimal size>." + JUCE base64  (alphabet ".A-Za-z0-9+/", LSB-first
           bit packing - NOT standard base64, standard decoding yields garbage)
  outer  : <?xml ...?> <VST3PluginState><IComponent>ENCODED</IComponent></VST3PluginState>
  final  : b"VC2!" + uint32 length + that outer XML
"""
import re
import struct
from pathlib import Path

ALPHA = ".ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"
MAGIC = b"VC2!"
SETTINGS = Path.home() / "Library/Application Support/CustomAmpHead JConcepcion.settings"


def juce_b64_decode(s):
    size, _, b64 = s.partition(".")
    n = int(size)
    buf = bytearray(n)
    for i, ch in enumerate(b64):
        v = ALPHA.index(ch)
        for b in range(6):
            if v & (1 << b):
                bit = i * 6 + b
                if bit // 8 < n:
                    buf[bit // 8] |= 1 << (bit % 8)
    return bytes(buf)


def juce_b64_encode(data):
    n = len(data)
    nchars = ((n << 3) + 5) // 6
    out = []
    for i in range(nchars):
        bit = i * 6
        idx, off = bit >> 3, bit & 7
        v = (data[idx] >> off) & 0x3F
        if off > 2 and idx + 1 < n:
            v |= (data[idx + 1] << (8 - off)) & 0x3F
        out.append(ALPHA[v])
    return f"{n}." + "".join(out)


def wrap(payload):
    return MAGIC + struct.pack("<I", len(payload)) + payload


def unwrap(blob):
    assert blob[:4] == MAGIC, "not a JUCE binary block"
    ln = struct.unpack_from("<I", blob, 4)[0]
    return blob[8:8 + ln]


def his_inner_xml():
    """The APVTS XML out of his saved standalone state."""
    raw = SETTINGS.read_text(errors="replace")
    m = re.search(r'name="filterState"\s+val="([^"]+)"', raw)
    if not m:
        raise SystemExit(f"no filterState in {SETTINGS}")
    return unwrap(juce_b64_decode(m.group(1))).decode("utf-8", "replace")


def set_ir_paths(xml, path_a, path_b):
    """Point IRFILES at files that exist. Everything else is left alone."""
    for p in (path_a, path_b):
        if p and not Path(p).is_file():
            raise SystemExit(f"IR not found, refusing to build a state that will "
                             f"silently fall back to head-only mode: {p}")
    esc = lambda s: (s or "").replace("&", "&amp;").replace('"', "&quot;")
    node = f'<IRFILES a="{esc(path_a)}" b="{esc(path_b)}" dir="{esc(str(Path(path_a).parent) if path_a else "")}"/>'
    if "<IRFILES" in xml:
        return re.sub(r"<IRFILES[^>]*/>", node, xml)
    return xml.replace("</", node + "</", 1) if "</" in xml else xml + node


def build_raw_state(xml_inner):
    encoded = juce_b64_encode(wrap(xml_inner.encode("utf-8")))
    outer = ('<?xml version="1.0" encoding="UTF-8"?> <VST3PluginState>'
             f"<IComponent>{encoded}</IComponent></VST3PluginState> ")
    return wrap(outer.encode("utf-8"))


def his_state_with_irs(path_a, path_b):
    return build_raw_state(set_ir_paths(his_inner_xml(), path_a, path_b))


if __name__ == "__main__":
    import sys
    a, b = (sys.argv[1], sys.argv[2]) if len(sys.argv) > 2 else ("", "")
    xml = set_ir_paths(his_inner_xml(), a, b) if a else his_inner_xml()
    print(re.search(r"<IRFILES[^>]*/>", xml).group(0) if "<IRFILES" in xml else "no IRFILES")
    print("raw_state bytes:", len(build_raw_state(xml)))
