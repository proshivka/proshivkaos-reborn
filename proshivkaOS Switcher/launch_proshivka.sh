#!/system/bin/sh
setenforce 0

exec >/data/local/tmp/proshivka.log 2>&1
echo "=== proshivkaOS Reborn Takeover Started ==="

# Image source determination
IMG_SRC="$1"
if [ -z "$IMG_SRC" ] || [ ! -f "$IMG_SRC" ]; then
    if [ -f "/data/user/0/com.proshivka.switcher/files/proshivka.img" ]; then
        IMG_SRC="/data/user/0/com.proshivka.switcher/files/proshivka.img"
    elif [ -f "/data/data/com.proshivka.switcher/files/proshivka.img" ]; then
        IMG_SRC="/data/data/com.proshivka.switcher/files/proshivka.img"
    elif [ -f "/data/local/tmp/proshivka.img" ]; then
        IMG_SRC="/data/local/tmp/proshivka.img"
    elif [ -f "/data/user/0/os.lowyk.launcher/files/lowyk.img" ]; then
        IMG_SRC="/data/user/0/os.lowyk.launcher/files/lowyk.img"
    elif [ -f "/data/local/tmp/lowyk.img" ]; then
        IMG_SRC="/data/local/tmp/lowyk.img"
    fi
fi

echo "Using image source: $IMG_SRC"

# Clean any stale exit requests
rm -f /data/local/tmp/lowyk_root/exit_proshivka
rm -f /data/local/tmp/exit_proshivka
rm -f /data/local/tmp/lowyk_root/exit_lowyk
rm -f /data/local/tmp/exit_lowyk

# Mount image if not already mounted
mkdir -p /data/local/tmp/lowyk_root
if ! mount | grep -q " /data/local/tmp/lowyk_root "; then
    echo "Mounting $IMG_SRC..."
    mount -o loop,rw "$IMG_SRC" /data/local/tmp/lowyk_root
fi

# Ensure bind mounts
mkdir -p /data/local/tmp/lowyk_root/dev
mkdir -p /data/local/tmp/lowyk_root/sys
mkdir -p /data/local/tmp/lowyk_root/proc

mount -o bind /dev /data/local/tmp/lowyk_root/dev
mount -o bind /sys /data/local/tmp/lowyk_root/sys
mount -o bind /proc /data/local/tmp/lowyk_root/proc

# Permissions for DRM & Backlight
chmod 666 /data/local/tmp/lowyk_root/dev/dri/card* 2>/dev/null
chmod 666 /dev/dri/card* 2>/dev/null
chmod 666 /sys/class/leds/lcd-backlight/brightness 2>/dev/null
chmod 666 /sys/devices/platform/soc/soc:mtk-leds/leds/lcd-backlight/brightness 2>/dev/null

touch /data/local/tmp/lowyk_root/skip_usb

# DO NOT USE 'stop' (it kills adbd on AOSP!)
echo "Stopping SurfaceFlinger & HWComposer (preserving adbd)..."
setprop ctl.stop surfaceflinger

GRAPHICS_SVCS=$(getprop | grep -E 'init\.svc\..*composer|init\.svc\..*pq' | sed 's/.*\[init\.svc\.\(.*\)\]:.*/\1/')
echo "Stopping Graphics services: $GRAPHICS_SVCS"
for s in $GRAPHICS_SVCS; do
    setprop ctl.stop "$s"
done

pkill -9 -f "graphics.composer" 2>/dev/null
pkill -9 -f "pq_aidl" 2>/dev/null
pkill -9 bootanimation 2>/dev/null
sleep 1

echo "Starting proshivkaOS..."
chroot /data/local/tmp/lowyk_root /init

echo "proshivkaOS ended, restoring Android graphics and framework..."
rm -f /data/local/tmp/lowyk_root/exit_proshivka
rm -f /data/local/tmp/exit_proshivka
rm -f /data/local/tmp/lowyk_root/exit_lowyk
rm -f /data/local/tmp/exit_lowyk

# Clean unmounts
umount -l /data/local/tmp/lowyk_root/dev 2>/dev/null
umount -l /data/local/tmp/lowyk_root/sys 2>/dev/null
umount -l /data/local/tmp/lowyk_root/proc 2>/dev/null
umount -l /data/local/tmp/lowyk_root 2>/dev/null

# Clean restart of graphics stack
for s in $GRAPHICS_SVCS; do
    setprop ctl.start "$s"
done
sleep 1
setprop ctl.start surfaceflinger
echo "=== Android Restored ==="
