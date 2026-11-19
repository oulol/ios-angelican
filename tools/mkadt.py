#!/usr/bin/env python3
"""Generate the Angelican Apple Device Tree from the Redmi 9C DTB.

Usage: python3 tools/mkadt.py DTB OUT.adt [options]

The DTB supplies RAM, CPU topology, GIC and generic-timer data. Display
geometry and framebuffer address are supplied separately because the stock
Angelica DTB does not describe them.
"""
import argparse
import os
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
sys.path.insert(0, str(HERE))
import adt  # noqa: E402

# Confirmed from the Redmi 9C NFC shim/device diagnostics. These are not in
# the stock DTB's mtkfb node.
DEFAULT_FB_BASE = 0x7EC50000
DEFAULT_FB_SIZE = 0xD8C000
DEFAULT_FB_WIDTH = 736       # 720 visible pixels, 32-pixel aligned stride
DEFAULT_FB_HEIGHT = 1600
DEFAULT_FB_STRIDE = 2944
DEFAULT_SOC_BASE = 0x01000000
DEFAULT_PANIC_LOG_SIZE = 0x100000
DEFAULT_PAGE_SIZE = 0x4000   # Existing mkpayload.py default; override explicitly if needed.
BOOT_MANIFEST_HASH = __import__("hashlib").sha384(b"angelican boot manifest").digest()

TOK_BEGIN_NODE, TOK_END_NODE, TOK_PROP, TOK_NOP, TOK_END = 1, 2, 3, 4, 9


class FDTNode:
    def __init__(self, name=""):
        self.name = name
        self.props = {}
        self.children = []

    def walk(self):
        yield self
        for child in self.children:
            yield from child.walk()

    def prop(self, key, default=None):
        return self.props.get(key, default)

    def strings(self, key):
        value = self.prop(key, b"")
        return [x.decode("ascii", "replace") for x in value.rstrip(b"\0").split(b"\0") if x]


def parse_fdt(blob):
    if len(blob) < 40:
        raise ValueError("DTB is too small")
    fields = struct.unpack_from(">10I", blob, 0)
    magic, total, off_struct, off_strings, off_reserve, version, last, bootcpu, size_strings, size_struct = fields
    if magic != 0xD00DFEED:
        raise ValueError(f"not a flattened device tree (magic 0x{magic:08x})")
    if total > len(blob):
        raise ValueError(f"DTB header says {total} bytes, file has {len(blob)}")
    strings = blob[off_strings:off_strings + size_strings]
    struct_data = blob[off_struct:off_struct + size_struct]
    root = None
    stack = []
    off = 0

    def cstring(data, pos):
        end = data.find(b"\0", pos)
        if end < 0:
            raise ValueError("unterminated FDT string")
        return data[pos:end].decode("utf-8", "replace"), end + 1

    while off + 4 <= len(struct_data):
        token, = struct.unpack_from(">I", struct_data, off)
        off += 4
        if token == TOK_BEGIN_NODE:
            name, off = cstring(struct_data, off)
            off = (off + 3) & ~3
            node = FDTNode(name)
            if stack:
                stack[-1].children.append(node)
            else:
                root = node
            stack.append(node)
        elif token == TOK_END_NODE:
            if not stack:
                raise ValueError("unbalanced FDT END_NODE")
            stack.pop()
        elif token == TOK_PROP:
            if not stack or off + 8 > len(struct_data):
                raise ValueError("malformed FDT property")
            length, nameoff = struct.unpack_from(">II", struct_data, off)
            off += 8
            if nameoff >= len(strings):
                raise ValueError("FDT property name offset out of range")
            name, _ = cstring(strings, nameoff)
            value = struct_data[off:off + length]
            if len(value) != length:
                raise ValueError("truncated FDT property")
            stack[-1].props[name] = value
            off = (off + length + 3) & ~3
        elif token == TOK_NOP:
            continue
        elif token == TOK_END:
            break
        else:
            raise ValueError(f"unknown FDT token {token} at offset 0x{off - 4:x}")
    if root is None or stack:
        raise ValueError("incomplete FDT structure")
    return root


def cells(data):
    if len(data) % 4:
        raise ValueError("property length is not a whole number of cells")
    return list(struct.unpack(">" + "I" * (len(data) // 4), data))


def read_number(raw, count):
    vals = cells(raw)
    if len(vals) < count:
        raise ValueError("reg property is shorter than its parent cell widths")
    value = 0
    for part in vals[:count]:
        value = (value << 32) | part
    return value


def parent_cell_count(node, key, default):
    # Filled by annotate_parents below.
    parent = getattr(node, "parent", None)
    raw = parent.prop(key) if parent else None
    return struct.unpack(">I", raw)[0] if raw and len(raw) == 4 else default


def annotate_parents(root):
    def visit(node, parent=None):
        node.parent = parent
        for child in node.children:
            visit(child, node)
    visit(root)


def reg_entries(node):
    raw = node.prop("reg")
    if raw is None:
        return []
    ac = parent_cell_count(node, "#address-cells", 2)
    sc = parent_cell_count(node, "#size-cells", 1)
    width = (ac + sc) * 4
    if width == 0 or len(raw) % width:
        return []
    out = []
    for off in range(0, len(raw), width):
        addr = read_number(raw[off:off + ac * 4], ac)
        size = read_number(raw[off + ac * 4:off + width], sc) if sc else 0
        out.append((addr, size))
    return out


def compatible(node, needle):
    return needle in node.strings("compatible")


def find_node(root, predicate, description):
    found = next((n for n in root.walk() if predicate(n)), None)
    if found is None:
        raise ValueError(f"could not find {description} in DTB")
    return found


def u32(*values):
    return struct.pack("<" + "I" * len(values), *values)


def u64(*values):
    return struct.pack("<" + "Q" * len(values), *values)


def cstr(value):
    return value.encode("ascii") + b"\0"


def make_node(name, props=(), children=()):
    node = adt.Node()
    node.props = [("name", 0, cstr(name))]
    node.props.extend((key, 0, value) for key, value in props)
    node.children = list(children)
    return node


def prop_phandle(counter):
    return ("AAPL,phandle", u32(next(counter)))


def extract_hardware(dtb_path):
    root = parse_fdt(adt.load(str(dtb_path)))
    annotate_parents(root)

    mem = find_node(root, lambda n: n.prop("device_type", b"").rstrip(b"\0") == b"memory",
                    "memory node")
    memory = reg_entries(mem)
    if len(memory) != 1:
        raise ValueError(f"expected one contiguous RAM region; found {memory!r}")
    ram_base, ram_size = memory[0]
    if not ram_size:
        raise ValueError("DTB RAM size is zero")

    cpus_node = find_node(root, lambda n: n.name == "cpus", "cpus node")
    cpu_nodes = []
    for n in cpus_node.children:
        if n.prop("device_type", b"").rstrip(b"\0") != b"cpu":
            continue
        entries = reg_entries(n)
        if not entries:
            continue
        mpidr = entries[0][0]
        cpu_nodes.append((mpidr, n))
    cpu_nodes.sort(key=lambda item: item[0])
    if not cpu_nodes:
        raise ValueError("no CPU nodes with reg properties found")

    gic_node = find_node(root, lambda n: compatible(n, "arm,gic-v3"),
                         "ARM GICv3 node")
    gic_regs = reg_entries(gic_node)
    if len(gic_regs) < 2:
        raise ValueError(f"GICv3 node needs distributor and redistributor reg entries; got {gic_regs!r}")
    gicd, gicd_size = gic_regs[0]
    gicr, gicr_size = gic_regs[1]

    timer = find_node(root, lambda n: compatible(n, "arm,armv8-timer"),
                      "ARMv8 generic timer node")
    freq_raw = timer.prop("clock-frequency")
    if not freq_raw or len(freq_raw) != 4:
        raise ValueError("generic timer has no 32-bit clock-frequency")
    timer_frequency, = struct.unpack(">I", freq_raw)
    intr = cells(timer.prop("interrupts", b""))
    # ARM GIC interrupt specifier: type, number, flags. PPI INTID = 16 + number.
    ppi = None
    for i in range(0, len(intr) - 2, 3):
        kind, number, flags = intr[i:i + 3]
        if kind == 1 and number == 11:
            ppi = 16 + number
            break
    if ppi is None:
        raise ValueError(f"could not identify virtual timer PPI 11 in interrupts={intr!r}")

    return {
        "ram_base": ram_base, "ram_size": ram_size,
        "cpus": cpu_nodes, "gicd": (gicd, gicd_size),
        "gicr": (gicr, gicr_size), "timer_frequency": timer_frequency,
        "vtimer_intid": ppi, "root": root,
    }


def build_angelica_adt(hw, args):
    ph = iter(range(1, 10000))
    dram_base = hw["ram_base"]
    dram_size = hw["ram_size"]
    panic_size = args.panic_log_size
    if panic_size < 0 or panic_size >= dram_size:
        raise ValueError("panic-log size must be nonnegative and smaller than RAM")
    panic_base = dram_base + dram_size - panic_size if panic_size else 0
    managed_size = dram_size - panic_size

    chosen_children = [make_node("memory-map", [prop_phandle(ph)])]
    chosen_props = [
        prop_phandle(ph),
        ("dram-base", u64(dram_base)),
        ("dram-size", u64(managed_size)),
        ("random-seed", os.urandom(64)),
        ("debug-enabled", u32(1)),
        ("embedded-panic-log-size", u32(panic_size)),
        ("firmware-version", cstr("angelican-shim")),
        ("system-firmware-version", cstr("angelican-shim")),
        ("boot-manifest-hash", BOOT_MANIFEST_HASH),
        ("crypto-hash-method", cstr("sha2-384")),
        ("display-scale", u32(args.display_scale)),
        # Synthetic identifiers: never copy a different device's ECID.
        ("unique-chip-id", u64(args.ecid)),
        ("chip-id", u32(args.chip_id)),
    ]
    chosen = make_node("chosen", chosen_props, chosen_children)

    cpu_children = []
    for logical_id, (mpidr, src) in enumerate(hw["cpus"]):
        cluster = (mpidr >> 8) & 0xff
        core = mpidr & 0xff
        cpu_children.append(make_node(f"cpu{logical_id}", [
            prop_phandle(ph),
            ("device_type", cstr("cpu")),
            ("compatible", cstr("arm,cortex-a53") + cstr("ARM,v8")),
            ("reg", u32(mpidr)),
            ("cpu-id", u32(logical_id)),
            ("cluster-id", u32(cluster)),
            ("cluster-type", u32(0)),
            ("die-cluster-id", u32(0)),
            ("cluster-core-id", u32(core)),
            ("l2-cache-id", u32(cluster)),
            ("state", cstr("running" if logical_id == 0 else "stopped")),
            ("timebase-frequency", u32(hw["timer_frequency"])),
        ]))
    cpus = make_node("cpus", [
        prop_phandle(ph), ("#address-cells", u32(1)), ("#size-cells", u32(0))
    ], cpu_children)

    gicd, gicd_size = hw["gicd"]
    gicr, gicr_size = hw["gicr"]
    gic = make_node("gic", [
        prop_phandle(ph),
        ("compatible", cstr("arm,gic-v3")),
        ("interrupt-controller", b""),
        ("virtual-timer-intid", u32(hw["vtimer_intid"])),
        ("reg", u64(gicd - args.soc_base, gicd_size,
                    gicr - args.soc_base, gicr_size)),
    ])
    arm_io = make_node("arm-io", [
        prop_phandle(ph),
        ("device_type", cstr("angelican-io")),
        ("compatible", cstr("angelican-io") + cstr("arm-io")),
        ("#address-cells", u32(2)),
        ("#size-cells", u32(2)),
        ("ranges", u64(0, args.soc_base, 0x100000000 - args.soc_base)),
    ], [gic, make_node("wdt", [prop_phandle(ph), ("no-pmu", u32(1))])])

    fbcon = make_node("angelican-fbcon", [
        prop_phandle(ph),
        ("reg", u64(args.fb_base, args.fb_size)),
        ("width", u32(args.fb_width)),
        ("height", u32(args.fb_height)),
        ("stride", u32(args.fb_stride)),
    ])
    pram = []
    if panic_size:
        pram = [make_node("pram", [
            prop_phandle(ph), ("reg", u64(panic_base, panic_size))
        ])]

    defaults = make_node("defaults", [
        prop_phandle(ph), ("no-effaceable-storage", u32(1))
    ])
    buttons = make_node("buttons", [
        prop_phandle(ph),
        ("button-names", cstr("volup") + cstr("voldown") + cstr("hold")),
        ("home-button-type", u32(0)),
    ])

    product = []
    # Reuse D22AP /product only if the user's local Apple ADT is available.
    # This is userland compatibility metadata, not Angelica hardware data.
    product_path = REPO / "research/ipsw-x/d22ap.adt"
    if product_path.exists():
        try:
            original, _ = adt.parse(adt.load(str(product_path)))
            product_node = next((n for n in original.children
                                 if n.prop("name", b"").split(b"\0")[0] == b"product"), None)
            if product_node:
                props = [(k, v) for k, flags, v in product_node.props
                         if not flags and k not in ("name", "AAPL,phandle")]
                product = [make_node("product", [prop_phandle(ph)] + props)]
        except Exception as exc:
            print(f"warning: could not import D22AP /product: {exc}", file=sys.stderr)

    root_props = [
        prop_phandle(ph),
        ("compatible", cstr("D22AP") + cstr("iPhone10,3") +
         cstr("oulol,angelican") + cstr("mediatek,mt6765") + cstr("AppleARM")),
        ("model", cstr("iPhone10,3")),
        ("target-type", cstr("D22")),
        ("target-sub-type", cstr("D22AP")),
        ("manufacturer", cstr("Apple Inc.")),
        ("#address-cells", u32(2)),
        ("#size-cells", u32(2)),
    ]
    children = [chosen, defaults, cpus, arm_io, fbcon, buttons] + pram + product
    return make_node("device-tree", root_props, children)



def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("out", type=Path, help="output Apple Device Tree (.adt)")
    ap.add_argument("--panic-log-size", type=lambda x: int(x, 0), default=DEFAULT_PANIC_LOG_SIZE)
    ap.add_argument("--soc-base", type=lambda x: int(x, 0), default=DEFAULT_SOC_BASE)
    ap.add_argument("--fb-base", type=lambda x: int(x, 0), default=DEFAULT_FB_BASE)
    ap.add_argument("--fb-size", type=lambda x: int(x, 0), default=DEFAULT_FB_SIZE)
    ap.add_argument("--fb-width", type=int, default=DEFAULT_FB_WIDTH)
    ap.add_argument("--fb-height", type=int, default=DEFAULT_FB_HEIGHT)
    ap.add_argument("--fb-stride", type=int, default=DEFAULT_FB_STRIDE)
    ap.add_argument("--display-scale", type=int, default=2)
    ap.add_argument("--ecid", type=lambda x: int(x, 0), default=0x414E47454C494341)
    ap.add_argument("--chip-id", type=lambda x: int(x, 0), default=0x6765)
    ap.add_argument("--dump", action="store_true", help="dump the generated ADT")
    a = ap.parse_args()

    hw = extract_hardware("./unpacked/dtb")
    print(f"RAM: 0x{hw['ram_base']:x} + 0x{hw['ram_size']:x}")
    print(f"CPUs: {len(hw['cpus'])}; MPIDRs: " +
          ", ".join(f"0x{x[0]:x}" for x in hw["cpus"]))
    print(f"GICD: 0x{hw['gicd'][0]:x} size 0x{hw['gicd'][1]:x}")
    print(f"GICR: 0x{hw['gicr'][0]:x} size 0x{hw['gicr'][1]:x}")
    print(f"Generic timer: {hw['timer_frequency']} Hz; virtual timer INTID {hw['vtimer_intid']}")
    print("WARNING: reserved-memory carve-outs are not yet subtracted from dram-base/dram-size")

    blob = adt.serialize(build_angelica_adt(hw, a))
    a.out.parent.mkdir(parents=True, exist_ok=True)
    a.out.write_bytes(blob)
    if a.dump:
        adt.dump(adt.parse(blob)[0])
    print(f"{a.out}: {len(blob)} bytes")


if __name__ == "__main__":
    main()
