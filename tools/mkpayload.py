#!/usr/bin/env python3
"""Pack an XNU kernel Mach-O and an Apple Device Tree into ANGELXNU payload.

Usage: mkpayload.py KERNEL ADT OUT [--cmdline "..."] [--android UNPACKEDBOOTFOLDER]
"""
import argparse
import struct
from pathlib import Path
import gzip
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent))
import adt  # noqa: E402

LC_SEGMENT_64, LC_UNIXTHREAD, LC_DYLD_CHAINED_FIXUPS = 0x19, 0x5, 0x80000034
PAGE = 0x4000
BOOT_LINE_LENGTH = 608
CMDLINE = 0x48
SEG_TABLE = 0x2D0

# ---- Mach-O / payload packer ----

def parse_macho(buf):
    magic, cputype, cpusub, ftype, ncmds, sizeofcmds, flags, _ = struct.unpack_from("<8I", buf, 0)
    assert magic == 0xfeedfacf, "not a 64-bit Mach-O"
    segs, entry, fixups = [], None, None
    off = 32
    for _ in range(ncmds):
        cmd, size = struct.unpack_from("<II", buf, off)
        if cmd == LC_SEGMENT_64:
            name = buf[off + 8:off + 24].split(b"\0")[0].decode()
            vmaddr, vmsize, fileoff, filesize = struct.unpack_from("<4Q", buf, off + 24)
            segs.append(dict(name=name, vmaddr=vmaddr, vmsize=vmsize, fileoff=fileoff, filesize=filesize))
        elif cmd == LC_UNIXTHREAD:
            entry, = struct.unpack_from("<Q", buf, off + 16 + 32 * 8)
        elif cmd == LC_DYLD_CHAINED_FIXUPS:
            fixups = struct.unpack_from("<II", buf, off + 8)
        off += size
    return dict(cputype=cputype, cpusub=cpusub & 0xff, segs=segs, entry=entry, fixups=fixups)


def decode(raw, fmt, image_base):
    if fmt in (2, 6):
        assert not raw >> 63, "bind in kernel"
        target = raw & ((1 << 36) - 1)
        high8 = (raw >> 36) & 0xff
        nxt = (raw >> 51) & 0xfff
        if fmt == 6:
            target += image_base
        return target | (high8 << 56), nxt, 4
    if fmt == 8:
        target = raw & ((1 << 30) - 1)
        nxt = (raw >> 51) & 0xfff
        return image_base + target, nxt, 4
    if fmt in (1, 7, 9, 12):
        auth = raw >> 63
        nxt = (raw >> 51) & 0x7ff
        stride = 4 if fmt in (7, 12) else 8
        if auth:
            target = raw & 0xffffffff
        else:
            target = raw & ((1 << 43) - 1)
            target |= ((raw >> 43) & 0xff) << 56
        if fmt in (7, 9, 12) or auth:
            target += image_base
        return target, nxt, stride
    raise ValueError(f"unsupported chained pointer format {fmt}")


def apply_chained_fixups(buf, m):
    if not m["fixups"]:
        return 0
    data_off, _ = m["fixups"]
    _, starts_off, _, _, imports_count, _, _ = struct.unpack_from("<7I", buf, data_off)
    assert imports_count == 0, "kernel has binds; only rebases are supported"
    image_base = m["segs"][0]["vmaddr"]
    seg_count, = struct.unpack_from("<I", buf, data_off + starts_off)
    nfix = 0
    for i in range(seg_count):
        seg_info, = struct.unpack_from("<I", buf, data_off + starts_off + 4 + 4 * i)
        if not seg_info:
            continue
        base = data_off + starts_off + seg_info
        size, page_size, fmt, seg_offset, max_valid, page_count = struct.unpack_from("<IHHQIH", buf, base)
        starts = struct.unpack_from("<%dH" % page_count, buf, base + 22)
        seg = m["segs"][i]
        for p, start in enumerate(starts):
            if start == 0xffff:
                continue
            assert not start & 0x8000, "DYLD_CHAINED_PTR_START_MULTI unsupported"
            loc = seg["fileoff"] + p * page_size + start
            while True:
                raw, = struct.unpack_from("<Q", buf, loc)
                target, nxt, stride = decode(raw, fmt, image_base)
                struct.pack_into("<Q", buf, loc, target)
                nfix += 1
                if not nxt:
                    break
                loc += nxt * stride
    return nfix


def add_ramdisk_to_adt(adt_blob, phys, size):
    root, _ = adt.parse(adt_blob)
    chosen = next(c for c in root.children if c.name == "chosen")
    mmap = next(c for c in chosen.children if c.name == "memory-map")
    assert mmap.prop("RAMDisk") is None, "ADT already has RAMDisk"
    mmap.props.append(("RAMDisk", 0, struct.pack("<2Q", phys, size)))
    return adt.serialize(root)


def adt_dram(adt_blob):
    root, _ = adt.parse(adt_blob)
    chosen = next(c for c in root.children if c.name == "chosen")
    return struct.unpack("<Q", chosen.prop("dram-base"))[0], struct.unpack("<Q", chosen.prop("dram-size"))[0]

def load_android_files(android_path):
    path = Path(android_path)

    if not path.is_dir():
        raise ValueError(
            f"--android must point to an unpacked boot image directory: {path}"
        )

    def find_file(candidates, label):
        found = [path / name for name in candidates if (path / name).is_file()]

        if not found:
            raise FileNotFoundError(
                f"Android {label} not found in {path}. "
                f"Expected one of: {', '.join(candidates)}"
            )

        if len(found) > 1:
            raise ValueError(
                f"Multiple Android {label} files found: "
                f"{', '.join(p.name for p in found)}"
            )

        return found[0]

    kernel_path = find_file(("kernel", "Image"), "kernel")
    ramdisk_path = find_file(
        ("ramdisk", "ramdisk.cpio", "ramdisk.img"),
        "ramdisk"
    )

    with gzip.open(kernel_path) as f:
        kernel = f.read()
    ramdisk = ramdisk_path.read_bytes()

    if not kernel:
        raise ValueError("Android kernel is empty")

    if not ramdisk:
        raise ValueError("Android ramdisk is empty")

    print(f"Android kernel:  {kernel_path} ({len(kernel)} bytes)")
    print(f"Android ramdisk: {ramdisk_path} ({len(ramdisk)} bytes)")

    return kernel, ramdisk

def pack_macho(
    kernel_path,
    adt_blob,
    cmdline,
    page_size=PAGE,
    android_path=None,
):
    android_files = (
        load_android_files(android_path)
        if android_path is not None
        else None
    )
    buf = bytearray(Path(kernel_path).read_bytes())
    m = parse_macho(buf)
    nfix = apply_chained_fixups(buf, m)
    segs = [s for s in m["segs"] if s["vmsize"]]
    text = m["segs"][0]
    assert text["name"] == "__TEXT" and text["fileoff"] == 0
    virt_base = text["vmaddr"] & ~(0x2000000 - 1)
    kernel_end = max(s["vmaddr"] + s["vmsize"] for s in segs)
    kernel_end = (kernel_end + page_size - 1) & ~(page_size - 1)
    phys_base, mem_size = adt_dram(adt_blob)
    pieces = [(s["vmaddr"], s["vmsize"], buf[s["fileoff"]:s["fileoff"] + s["filesize"]]) for s in segs]

    hdr_size = SEG_TABLE + 32 * len(pieces)
    out = bytearray(hdr_size)

    seg_table = []

    # Append XNU segments.
    for vmaddr, vmsize, data in pieces:
        off = len(out)
        out += data
        out += b"\0" * (-len(out) & 0xf)

        seg_table.append((vmaddr, vmsize, off, len(data)))

    # Append the Apple Device Tree.
    adt_off = len(out)
    out += adt_blob
    out += b"\0" * (-len(out) & 0xf)

    # Append Android kernel and ramdisk.
    android_kernel_offset = 0
    android_kernel_size = 0
    android_ramdisk_offset = 0
    android_ramdisk_size = 0

    if android_files is not None:
        android_kernel, android_ramdisk = android_files

        android_ramdisk_offset = len(out)
        android_ramdisk_size = len(android_ramdisk)
        out += android_ramdisk
        out += b"\0" * (-len(out) & 0xf)

        ANDROID_IMAGE_ALIGNMENT = 0x200000
        PAYLOAD_PHYS = 0x51B00000

        # Append Android kernel.
        android_text_offset, = struct.unpack_from("<Q", android_kernel, 0x08)

        current_phys = PAYLOAD_PHYS + len(out)

        # Align the implied ARM64 Image base, not the Image start.
        aligned_base = (
            (current_phys - android_text_offset + ANDROID_IMAGE_ALIGNMENT - 1)
            & ~(ANDROID_IMAGE_ALIGNMENT - 1)
        )

        android_kernel_offset = (
            aligned_base + android_text_offset - PAYLOAD_PHYS
        )

        assert android_kernel_offset >= len(out)
        assert aligned_base % ANDROID_IMAGE_ALIGNMENT == 0

        android_kernel_size = len(android_kernel)
        out += b"\0" * (android_kernel_offset - len(out))
        out += android_kernel
        out += b"\0" * (-len(out) & 0xf)

        print(f"Android Image base: {aligned_base:#x}")
        print(f"Android Image start: {PAYLOAD_PHYS + android_kernel_offset:#x}")
        print(f"Android text_offset: {android_text_offset:#x}")

    # Payload header.
    struct.pack_into(
        "<8sII",
        out,
        0,
        b"ANGELXNU",
        1,
        len(pieces),
    )

    struct.pack_into(
        "<7Q",
        out,
        0x10,
        virt_base,
        m["entry"],
        adt_off,
        len(adt_blob),
        kernel_end,
        phys_base,
        mem_size,
    )

    cl = cmdline.encode()[:BOOT_LINE_LENGTH - 1]
    out[CMDLINE:CMDLINE + len(cl)] = cl

    # Android metadata:
    # 0x2A8: has_android
    # 0x2A9: reserved[7]
    # 0x2B0: android_kernel_offset
    # 0x2B8: android_kernel_size
    # 0x2C0: android_ramdisk_offset
    # 0x2C8: android_ramdisk_size

    struct.pack_into(
        "<B",
        out,
        0x2A8,
        int(android_files is not None),
    )

    out[0x2A9:0x2B0] = b"\0" * 7

    struct.pack_into(
        "<4Q",
        out,
        0x2B0,
        android_kernel_offset,
        android_kernel_size,
        android_ramdisk_offset,
        android_ramdisk_size,
    )

    # Segment table starts at 0x2D0.
    for i, entry in enumerate(seg_table):
        struct.pack_into(
            "<4Q",
            out,
            SEG_TABLE + 32 * i,
            *entry,
        )
    info = dict(
        cputype=hex(m["cputype"]), cpusub=m["cpusub"], fixups=nfix, virt_base=hex(virt_base),
        entry=hex(m["entry"]), kernel_end=hex(kernel_end), size=len(out),
        segs=[(s["name"], hex(s["vmaddr"]), hex(s["vmsize"])) for s in segs],
        android=android_files is not None,
        android_kernel_offset=hex(android_kernel_offset),
        android_kernel_size=android_kernel_size,
        android_ramdisk_offset=hex(android_ramdisk_offset),
        android_ramdisk_size=android_ramdisk_size
    )
    return bytes(out), info, adt_blob



def build(
    kernel_path,
    adt_path,
    cmdline,
    page_size=PAGE,
    android_path=None,
):
    adt_blob = Path(adt_path).read_bytes()

    blob, info, _final_adt = pack_macho(
        kernel_path,
        adt_blob,
        cmdline,
        page_size,
        android_path,
    )

    return blob, info


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("kernel", type=Path)
    ap.add_argument("adt", type=Path)
    ap.add_argument("out", type=Path)
    ap.add_argument("--android", type=Path, help="path to unpacked boot.img. leave empty to not include Android.")
    ap.add_argument("--cmdline", default="debug=0xc serial=3 cpus=1 keepsyms=1")
    ap.add_argument("--page-size", type=lambda x: int(x, 0), default=PAGE)
    a = ap.parse_args()
    if a.page_size not in (0x1000, 0x4000, 0x10000):
        ap.error("--page-size must be 0x1000, 0x4000, or 0x10000")
    for path, label in ((a.kernel, "kernel"), (a.adt, "ADT")):
        if not path.is_file():
            ap.error(f"{label} does not exist: {path}")
    if a.android is not None and not a.android.is_dir():
        ap.error(f"Android directory does not exist: {a.android}")
    blob, info = build(
        a.kernel,
        a.adt,
        a.cmdline,
        a.page_size,
        a.android,
    )
    a.out.parent.mkdir(parents=True, exist_ok=True)
    a.out.write_bytes(blob)
    for key, value in info.items():
        if key != "segs":
            print(f"{key}: {value}")
    for segment in info["segs"]:
        print("  ", *segment)
    print(f"Payload written: {a.out}")


if __name__ == "__main__":
    main()
