# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

# OpenWrt/LuCI feed recipe for luci-app-reac-aes67 (client-side JS app).
include $(TOPDIR)/rules.mk

PKG_NAME:=luci-app-reac-aes67
PKG_VERSION:=0.2.1
PKG_RELEASE:=1
PKG_MAINTAINER:=Pau Aliagas <linuxnow@gmail.com>
PKG_LICENSE:=GPL-3.0-or-later

LUCI_TITLE:=LuCI app for the REAC to AES67 bridge
LUCI_DEPENDS:=+reac-aes67
LUCI_PKGARCH:=all

# Standard LuCI app build (luci.mk handles htdocs/ + root/ install).
include $(TOPDIR)/feeds/luci/luci.mk

# call BuildPackage - OpenWrt buildroot signature to be discarded
