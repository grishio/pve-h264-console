#!/bin/bash
# SPDX-License-Identifier: MIT
# Remove the PVE H.264 console and restore stock Proxmox behaviour.
#
# usage: ./uninstall.sh [--repair]
#   --repair  if any Proxmox file still differs from its package afterwards,
#             reinstall the owning packages to restore the exact stock files
set -uo pipefail

REPAIR=0
[ "${1:-}" = "--repair" ] && REPAIR=1

PKGS="qemu-server pve-manager libpve-http-server-perl"
# exact installed versions: a plain "apt install --reinstall pkg" would
# silently UPGRADE to the newest available version (and its dependencies)
pinned() { for p in $PKGS; do printf '%s=%s ' "$p" "$(dpkg-query -W -f='${Version}' "$p")"; done; }
FILES=(
    /usr/share/perl5/PVE/QemuServer.pm
    /usr/share/perl5/PVE/API2/Qemu.pm
    /usr/share/perl5/PVE/Service/pveproxy.pm
    /usr/share/perl5/PVE/APIServer/AnyEvent.pm
    /usr/share/pve-manager/js/pvemanagerlib.js
)
rc=0

echo ">> stopping H.264 console daemons"
for u in $(systemctl list-units --plain --no-legend 'pve-h264d@*' | awk '{print $1}'); do
    systemctl stop "$u"
done

echo ">> reverting Proxmox integration"
if [ -x /usr/local/sbin/pve-h264-patch ]; then
    /usr/local/sbin/pve-h264-patch revert || rc=1
else
    echo "   patcher not installed - nothing to revert (files are checked below)"
fi

echo ">> removing installed files"
rm -f /etc/apt/apt.conf.d/99pve-h264-console /usr/local/sbin/pve-h264-patch \
      /usr/local/sbin/pve-h264d /etc/systemd/system/pve-h264d@.service
rm -rf /usr/share/pve-h264-console /usr/share/doc/pve-h264-console /run/pve-h264
systemctl daemon-reload

echo ">> verifying Proxmox files against their packages"
leftover=$(grep -l "pve-h264-console BEGIN" "${FILES[@]}" 2>/dev/null || true)
modified=$(dpkg --verify $PKGS 2>/dev/null | awk '{print $NF}' | grep -Fx -f <(printf '%s\n' "${FILES[@]}") || true)
if [ -z "$leftover" ] && [ -z "$modified" ]; then
    echo "   all ${#FILES[@]} files match the installed packages"
else
    [ -n "$leftover" ] && echo "   WARNING: H.264 blocks still present in:" && printf '     %s\n' $leftover
    [ -n "$modified" ] && echo "   WARNING: differ from the package version:" && printf '     %s\n' $modified
    if [ "$REPAIR" = 1 ]; then
        echo ">> --repair: reinstalling the installed versions: $(pinned)"
        if DEBIAN_FRONTEND=noninteractive apt-get install -y -q --reinstall --no-install-recommends $(pinned); then
            systemctl reload-or-restart pvedaemon pveproxy
            modified=$(dpkg --verify $PKGS 2>/dev/null | awk '{print $NF}' | grep -Fx -f <(printf '%s\n' "${FILES[@]}") || true)
            if [ -z "$modified" ]; then echo "   repaired: all files match"; else echo "   still differing:"; printf '     %s\n' $modified; rc=1; fi
        else
            echo "   reinstall failed - the installed version may no longer be downloadable;"
            echo "   nothing was upgraded. Fix the files by hand or after a regular update."; rc=1
        fi
    else
        echo "   restore the exact package files (same versions, no upgrade) with:"
        echo "     apt install --reinstall $(pinned)"
        echo "   (or re-run: ./uninstall.sh --repair)"
        rc=1
    fi
fi

echo
echo "Removed. /etc/pve-h264 kept (delete manually)."
echo "Running VMs keep '-display dbus' until their next full stop/start (harmless)."
echo "Reload open Proxmox web UI tabs."
exit $rc
