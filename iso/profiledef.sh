#!/usr/bin/env bash
# shellcheck disable=SC2034

iso_name="luksarchy"
iso_label="LUKSARCHY_$(date --date="@${SOURCE_DATE_EPOCH:-$(date +%s)}" +%Y%m)"
iso_publisher="Luks <https://github.com/luks>"
iso_application="Luksarchy Installer"
iso_version="$(date --date="@${SOURCE_DATE_EPOCH:-$(date +%s)}" +%Y.%m.%d)"
install_dir="arch"
buildmodes=('iso')
bootmodes=('bios.syslinux' 'uefi.systemd-boot')
arch="x86_64"
pacman_conf="pacman.conf"
airootfs_image_type="squashfs"
airootfs_image_tool_options=('-comp' 'zstd' '-Xcompression-level' '15' '-b' '512K')
bootstrap_tarball_compression=('zstd' '-c' '-T0' '--auto-threads=logical' '--long' '-19')
file_permissions=(
  ["/root/.automated_script.sh"]="0:0:755"
  ["/usr/local/bin/luksarchy"]="0:0:755"
  ["/etc/shadow"]="0:0:400"
  ["/etc/gshadow"]="0:0:400"
)
