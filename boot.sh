#!/bin/bash
DIR="$(cd "$(dirname "$0")" && pwd)"
ISO_SRC="$DIR/newrex.iso"
ISO_DEST="/mnt/c/Users/Public/newrex.iso"
HDD_PATH="C:\\Users\\Public\\hdd.img"

echo "===================================================="
echo " [RexFS] Starting Newrex OS in QEMU"
echo "===================================================="
echo " ISO Source   : $ISO_SRC"
echo " ISO Windows  : $ISO_DEST"
echo " HDD Path     : $HDD_PATH"
echo " Build Time   : $(stat -c %y "$ISO_SRC" 2>/dev/null || date -r "$ISO_SRC" 2>/dev/null || echo "Unknown")"
echo "===================================================="

# Sync latest WSL build to Public folder for Windows native QEMU
cp "$ISO_SRC" "$ISO_DEST"

# Execute QEMU from the Public folder so relative paths and serial.log work
cd /mnt/c/Users/Public
/mnt/c/Program\ Files/qemu/qemu-system-i386.exe \
    -cdrom "newrex.iso" \
    -drive file="hdd.img",format=raw,index=0,media=disk \
    -boot d \
    -cpu qemu32,vendor=AuthenticAMD \
    -netdev user,id=net0 \
    -device rtl8139,netdev=net0 \
    -chardev file,id=char0,path=serial.log \
    -serial chardev:char0

