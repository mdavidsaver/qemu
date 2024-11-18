#!/bin/sh
set -e -x

MYDIR="$(dirname "$(readlink -f "$0")" )"

usage() {
    cat <<EOF
Usage: $0 <prog.elf> <eeprom.img> [qemu args ...]

Create eeprom image file with ./make-ee.py script.
EOF
    exit 1
}

ELF="$1"
EEPROM="$2"
shift
shift

[ -f "$ELF" -a -f "$EEPROM" ] || usage

exec "$MYDIR"/build/qemu-system-arm \
 -M xilinx-zynq-a9 \
 -monitor stdio \
 -serial null \
 -serial vc \
 -kernel "$ELF" \
 -blockdev driver=file,node-name=eeprom,filename="$EEPROM" \
 -device at24c-eeprom,bus=i2c-bus.0,address=0x50,rom-size=256,drive=eeprom \
 -nic user,hostfwd=tcp::3000-:3000 \
 -nic user,hostfwd=tcp::3007-:7 \
 -nic user,hostfwd=tcp::3017-:17 \
 -nic user,hostfwd=tcp::3027-:27 \
 -nic user,hostfwd=tcp::3037-:37 \
 -d unimp,guest_errors \
 -s \
 "$@"
