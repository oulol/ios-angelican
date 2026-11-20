
#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")"

# ---------------------------------------------------------------------------
# Arguments
# ---------------------------------------------------------------------------

if (( $# > 1 )); then
    echo "Usage: $0 [payload.bin]" >&2
    exit 1
fi

CALLER_DIR="$PWD"
PAYLOAD="${1:-}"

if [[ -n "$PAYLOAD" && "$PAYLOAD" != /* ]]; then
    PAYLOAD="$CALLER_DIR/$PAYLOAD"
fi

# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------

OUT="out"
OBJ_DIR="$OUT/obj"
DTB="../unpacked/dtb"
OUTPUT="$OUT/shim-boot.img"

mkdir -p "$OBJ_DIR"

# ---------------------------------------------------------------------------
# Redmi 9C stock boot-image parameters
# ---------------------------------------------------------------------------

BASE=0x40000000

KERNEL_OFFSET=0x00080000
RAMDISK_OFFSET=0x11B00000
TAGS_OFFSET=0x07880000
DTB_OFFSET=0x07880000

PAGE_SIZE=2048

HEADER_VERSION=2
OS_VERSION="13.0.0"
OS_PATCH_LEVEL="2024-04"

CMDLINE="bootopt=64S3,32N2,64N2 androidboot.selinux=permissive androidboot.init_fatal_reboot_target=recovery kpti=off buildvariant=userdebug"

# ---------------------------------------------------------------------------
# Toolchain
# ---------------------------------------------------------------------------

CC="${CROSS_COMPILE:-aarch64-linux-gnu-}gcc"
OBJCOPY="${CROSS_COMPILE:-aarch64-linux-gnu-}objcopy"
NM="${CROSS_COMPILE:-aarch64-linux-gnu-}nm"
READELF="${CROSS_COMPILE:-aarch64-linux-gnu-}readelf"

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------

for tool in "$CC" "$OBJCOPY" "$NM" "$READELF" mkbootimg gzip cpio cp stat; do
    command -v "$tool" >/dev/null 2>&1 || {
        echo "Missing tool: $tool" >&2
        exit 1
    }
done

# ---------------------------------------------------------------------------
# Input validation
# ---------------------------------------------------------------------------

if [[ ! -f "$DTB" ]]; then
    echo "Missing DTB: $DTB" >&2
    exit 1
fi

if [[ -n "$PAYLOAD" && ! -f "$PAYLOAD" ]]; then
    echo "Missing payload: $PAYLOAD" >&2
    exit 1
fi

# ---------------------------------------------------------------------------
# Compiler flags
# ---------------------------------------------------------------------------

FLAGS=(
    -O2
    -ffreestanding
    -fno-builtin
    -fno-stack-protector
    -fno-pic
    -fno-pie
    -nostdlib
    -mgeneral-regs-only
    -mstrict-align
    -Wall
    -Wextra
)

# ---------------------------------------------------------------------------
# Discover sources
# ---------------------------------------------------------------------------

mapfile -t C_SOURCES < <(
    find . -maxdepth 1 -type f -name '*.c' | sort
)

mapfile -t ASM_SOURCES < <(
    find . -maxdepth 1 -type f -name '*.S' | sort
)

if (( ${#C_SOURCES[@]} == 0 && ${#ASM_SOURCES[@]} == 0 )); then
    echo "No source files found." >&2
    exit 1
fi

OBJECTS=()
C_OBJECTS=()

# ---------------------------------------------------------------------------
# Compile
# ---------------------------------------------------------------------------

echo "[1/6] Compiling sources..."

for SOURCE in "${ASM_SOURCES[@]}" "${C_SOURCES[@]}"; do
    BASENAME="$(basename "${SOURCE%.*}")"
    OBJECT="$OBJ_DIR/$BASENAME.o"

    echo "  $SOURCE -> $OBJECT"

    "$CC" "${FLAGS[@]}" -c "$SOURCE" -o "$OBJECT"

    OBJECTS+=("$OBJECT")

    if [[ "$SOURCE" == *.c ]]; then
        C_OBJECTS+=("$OBJECT")
    fi
done

# ---------------------------------------------------------------------------
# Check absolute relocations in C objects
# ---------------------------------------------------------------------------

echo
echo "[2/6] Checking relocations..."

ABS_RELOCS=0

for OBJECT in "${C_OBJECTS[@]}"; do
    COUNT="$("$READELF" -r "$OBJECT" |
        grep -c 'R_AARCH64_ABS64' || true)"

    ABS_RELOCS=$((ABS_RELOCS + COUNT))
done

if (( ABS_RELOCS != 0 )); then
    echo "ERROR: $ABS_RELOCS absolute relocations in C objects." >&2
    echo "The shim must be position-independent with respect to ABL placement." >&2
    exit 1
fi

# ---------------------------------------------------------------------------
# Link
# ---------------------------------------------------------------------------

echo
echo "[3/6] Linking..."

"$CC" "${FLAGS[@]}" \
    -static \
    -no-pie \
    -Wl,--build-id=none \
    -Wl,-T,start.ld \
    -Wl,-N \
    -Wl,-Map,"$OUT/shim.map" \
    "${OBJECTS[@]}" \
    -o "$OUT/shim.elf"

# ---------------------------------------------------------------------------
# Generate raw Image
# ---------------------------------------------------------------------------

echo
echo "[4/6] Generating Image..."

"$OBJCOPY" -O binary "$OUT/shim.elf" "$OUT/Image"

END_HEX="$("$NM" -n "$OUT/shim.elf" |
    awk '$3 == "_end" { print $1; exit }')"

if [[ -z "$END_HEX" ]]; then
    echo "ERROR: Missing _end symbol in linker output." >&2
    exit 1
fi

IMAGE_SIZE=$((16#$END_HEX))

truncate -s "$IMAGE_SIZE" "$OUT/Image"

# ---------------------------------------------------------------------------
# Prepare ramdisk
# ---------------------------------------------------------------------------

echo
echo "[5/6] Preparing ramdisk..."

if [[ -n "$PAYLOAD" ]]; then
    echo "Embedding payload: $PAYLOAD"
    cp "$PAYLOAD" "$OUT/ramdisk.bin"
else
    echo "Creating empty newc CPIO ramdisk..."
    (
        cd "$OUT"
        rm -rf rd
        mkdir rd
        cd rd
        find . | cpio -o -H newc --quiet | gzip -9 > ../ramdisk.bin
    )
fi

# ---------------------------------------------------------------------------
# Package boot image
# ---------------------------------------------------------------------------

echo
echo "Building Android boot image..."

gzip -n -9 -c "$OUT/Image" > "$OUT/kernel.gz"
mkbootimg \
    --header_version "$HEADER_VERSION" \
    --kernel "$OUT/kernel.gz" \
    --ramdisk "$OUT/ramdisk.bin" \
    --dtb "$DTB" \
    --base "$BASE" \
    --kernel_offset "$KERNEL_OFFSET" \
    --ramdisk_offset "$RAMDISK_OFFSET" \
    --tags_offset "$TAGS_OFFSET" \
    --dtb_offset "$DTB_OFFSET" \
    --pagesize "$PAGE_SIZE" \
    --os_version "$OS_VERSION" \
    --os_patch_level "$OS_PATCH_LEVEL" \
    --cmdline "$CMDLINE" \
    -o "$OUTPUT"

# ---------------------------------------------------------------------------
# Verification
# ---------------------------------------------------------------------------

echo
echo "[6/6] Checking..."

"$READELF" -h "$OUT/shim.elf"

echo
echo "Important symbols:"
"$NM" -n "$OUT/shim.elf" |
    grep -E '(_head|_start|main|loader_main|minimal_el1_main|el1_vectors|el1_stack|l1_table|_end)$' \
    || true

echo
echo "Build summary:"
echo "  C files:       ${#C_SOURCES[@]}"
echo "  Assembly:      ${#ASM_SOURCES[@]}"
echo "  Object files:  ${#OBJECTS[@]}"
echo "  Image size:    $IMAGE_SIZE bytes"
echo "  Ramdisk size:  $(stat -c '%s' "$OUT/ramdisk.bin") bytes"
echo "  DTB:           $DTB"
echo "  Command line:  $CMDLINE"
echo "  Boot image:    $OUTPUT"

echo
if command -v xxd >/dev/null 2>&1; then
    xxd -l 64 "$OUT/Image"
fi

echo
stat -c '%n: %s bytes' "$OUTPUT"