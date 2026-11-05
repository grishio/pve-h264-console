#!/bin/bash
# SPDX-License-Identifier: MIT
# Install / update the PVE H.264 console. Safe to re-run.
set -euo pipefail
cd "$(dirname "$(readlink -f "$0")")"

if ! pkg-config --exists libavcodec libswresample gio-2.0 2>/dev/null; then
    echo ">> installing build dependencies"
    DEBIAN_FRONTEND=noninteractive apt-get install -y -q \
        build-essential pkg-config libavcodec-dev libavutil-dev libswscale-dev libswresample-dev \
        libglib2.0-dev libva-dev
fi

echo ">> building pve-h264d"
make -C daemon -s
install -D -m 0755 daemon/pve-h264d /usr/local/sbin/pve-h264d

echo ">> installing web viewer"
install -d -m 0755 /usr/share/pve-h264-console
install -m 0644 www/index.html.tpl www/h264.js www/settings.js www/audio.js www/audio-worklet.js www/clipboard.js www/keymap.js www/h264.css /usr/share/pve-h264-console/

echo ">> installing systemd unit + config"
install -D -m 0644 systemd/pve-h264d@.service /etc/systemd/system/pve-h264d@.service
install -D -m 0644 documentation.md /usr/share/doc/pve-h264-console/documentation.md
install -d -m 0755 /etc/pve-h264
[ -e /etc/pve-h264/h264d.conf ] || install -m 0644 h264d.conf /etc/pve-h264/h264d.conf
systemctl daemon-reload

echo ">> installing patcher + apt hook"
install -m 0755 patches/pve-h264-patch /usr/local/sbin/pve-h264-patch
cat > /etc/apt/apt.conf.d/99pve-h264-console <<'HOOK'
// re-apply pve-h264-console integration after Proxmox package upgrades
DPkg::Post-Invoke { "if [ -x /usr/local/sbin/pve-h264-patch ]; then /usr/local/sbin/pve-h264-patch apply --quiet || echo 'pve-h264-console: re-apply failed, run pve-h264-patch status'; /usr/local/sbin/pve-h264-patch gpu-check --quiet || true; fi"; };
HOOK

echo ">> patching Proxmox VE"
PATCH_OK=1
/usr/local/sbin/pve-h264-patch apply || PATCH_OK=0
/usr/local/sbin/pve-h264-patch status || true

# restart running viewer daemons so they pick up the new binary
for u in $(systemctl list-units --plain --no-legend 'pve-h264d@*' | awk '{print $1}'); do
    systemctl stop "$u" || true
done
echo
if [ "$PATCH_OK" = 0 ]; then
    echo "WARNING: some Proxmox patches could not be applied (see above). Proxmox itself is"
    echo "unaffected, but parts of the H.264 console may be missing. Check: pve-h264-patch status"
fi
echo "Done. VMs must be fully stopped and started once to get '-display dbus'."
echo "Then reload the PVE web UI page (a normal reload is enough) and use VM -> Console (dropdown) -> 'H.264'."
