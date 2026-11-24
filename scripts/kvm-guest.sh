#!/bin/bash
# Runs a command inside a virtual machine whose kernel has udmabuf, so that Mesa's software renderers export their
# textures as DMA-BUFs there (GitHub's hosted runners' own kernel has no udmabuf): CI's check of plug-in pictures shared
# between processes from the GPU's memory.
#
#   scripts/kvm-guest.sh <folder for its files> <command> [arguments...]
#
# The guest is started by QEMU (Ubuntu's qemu-system-x86) with Ubuntu's generic kernel (linux-image-virtual and its
# linux-modules-extra, which hold udmabuf, 9p and overlayfs), with KVM where the machine allows it. It has no disk of
# its own: its root is this machine's root file system, read through 9p, under a tmpfs that takes what is written, so
# the command finds the build, Qt and every library where they are here, and nothing it writes outlasts it. The
# command runs as the guest's first process, as root, in the directory this script was started in; the guest's
# console is printed here, and this script ends with the command's exit status (1 if the guest never said one).
set -euo pipefail

work=$1
shift
[ $# -gt 0 ] || { echo "kvm-guest.sh: no command" >&2; exit 2; }

ver=$(ls /lib/modules 2> /dev/null | grep -- '-generic$' | sort -V | tail -n 1 || true)
[ -n "$ver" ] || { echo "kvm-guest.sh: no generic kernel is installed (apt: linux-image-virtual)" >&2; exit 1; }
kernel=/boot/vmlinuz-$ver
sudo test -r "$kernel" || { echo "kvm-guest.sh: $kernel is not there" >&2; exit 1; }
command -v qemu-system-x86_64 > /dev/null || { echo "kvm-guest.sh: qemu-system-x86_64 is not installed (apt: qemu-system-x86)" >&2; exit 1; }

mkdir -p "$work"
work=$(cd "$work" && pwd)

# An initramfs for that kernel with the modules its root needs, and a boot script that mounts it (initramfs-tools'
# boot=<script>, as its own nfs and local are): made from a copy of this machine's settings, which are left alone.
conf=$work/initramfs-tools
rm -rf "$conf"
cp -r /etc/initramfs-tools "$conf"
printf '9pnet_virtio\n9p\noverlay\n' >> "$conf/modules"
mkdir -p "$conf/scripts"
cat > "$conf/scripts/montage9p" << 'BOOT'
# The host's root file system through 9p, read-only, under a tmpfs for what the guest writes.
mountroot() {
    modprobe 9pnet_virtio
    modprobe 9p
    modprobe overlay
    mkdir -p /lower /upper
    mount -t 9p -o trans=virtio,version=9p2000.L,ro,msize=262144 hostroot /lower || panic "the host's root did not mount"
    mount -t tmpfs -o size=3g,mode=755 tmpfs /upper
    mkdir -p /upper/data /upper/work
    mount -t overlay -o lowerdir=/lower,upperdir=/upper/data,workdir=/upper/work overlay "${rootmnt}" || panic "no overlay over the host's root"
}
BOOT
sudo mkinitramfs -d "$conf" -o "$work/initrd.img" "$ver"

# The guest's first process: the command, then the machine switched off (its status is read from the console).
init=$work/init.sh
{
    echo '#!/bin/sh'
    echo 'export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin HOME=/root USER=root LANG=C.UTF-8'
    echo 'mount -t tmpfs tmpfs /tmp'
    echo 'mkdir -p /dev/shm /tmp/runtime && chmod 700 /tmp/runtime'
    echo 'mountpoint -q /dev/shm || mount -t tmpfs tmpfs /dev/shm'
    echo 'export XDG_RUNTIME_DIR=/tmp/runtime TMPDIR=/tmp'
    echo 'ip link set lo up 2> /dev/null'
    echo 'modprobe udmabuf 2> /dev/null'
    echo 'if [ -c /dev/udmabuf ]; then chmod 666 /dev/udmabuf; else echo "kvm-guest: the guest has no /dev/udmabuf"; fi'
    echo 'echo "kvm-guest: Linux $(uname -r), $(nproc) processors"'
    printf 'cd %q || echo "kvm-guest: the working directory is not there"\n' "$PWD"
    printf '%q ' "$@"
    echo
    echo 'echo "MONTAGE-GUEST-EXIT=$?"'
    echo 'sync'
    echo 'echo 1 > /proc/sys/kernel/sysrq'
    echo 'echo o > /proc/sysrq-trigger'
    echo 'sleep 120'
} > "$init"
chmod 755 "$init"

accel=tcg
if sudo test -r /dev/kvm && sudo test -w /dev/kvm; then accel=kvm:tcg; else echo "kvm-guest.sh: no /dev/kvm here: the guest is emulated, and slow"; fi
log=$work/console.log
set +e
sudo timeout 1500 qemu-system-x86_64 -machine q35,accel=$accel -cpu max -smp "$(nproc)" -m 6G -nographic -no-reboot -nic none \
    -kernel "$kernel" -initrd "$work/initrd.img" \
    -append "console=ttyS0 boot=montage9p root=hostroot rw init=$init panic=-1 loglevel=4" \
    -virtfs local,path=/,mount_tag=hostroot,security_model=none,readonly=on,multidevs=remap < /dev/null 2>&1 | tee "$log"
set -e
status=$(grep -a -o 'MONTAGE-GUEST-EXIT=[0-9]*' "$log" | tail -n 1 | cut -d= -f2 || true)
if [ -z "$status" ]; then
    echo "kvm-guest.sh: the guest ended without the command's status" >&2
    exit 1
fi
exit "$status"
