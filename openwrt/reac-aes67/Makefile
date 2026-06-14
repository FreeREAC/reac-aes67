# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

# OpenWrt feed recipe for the reac-aes67 daemon.
# Copy this directory into an OpenWrt feed (or package/) and build with the SDK.
# On OpenWrt 25.12 the SDK emits .apk packages.
include $(TOPDIR)/rules.mk

PKG_NAME:=reac-aes67
PKG_VERSION:=0.3.0
PKG_RELEASE:=1
PKG_MAINTAINER:=Pau Aliagas <linuxnow@gmail.com>
PKG_LICENSE:=GPL-3.0-or-later

# Point at the source repo. For local builds, use PKG_SOURCE_URL/PROTO=git or a
# tarball; here we assume the src/ tree is vendored alongside this recipe.
include $(INCLUDE_DIR)/package.mk

define Package/reac-aes67
  SECTION:=sound
  CATEGORY:=Sound
  TITLE:=REAC to AES67 bridge
  URL:=https://github.com/FreeREAC/reac-aes67
  DEPENDS:=+libreac +libubus +libblobmsg-json
endef

define Package/reac-aes67/description
  Decodes Roland REAC (EtherType 0x8819) captured on a LAN interface and
  re-emits it as AES67 (RTP L24 multicast) for PipeWire / AES67 receivers.
  One daemon instance per REAC stream, managed via UCI + procd, with live
  status over ubus.
endef

define Package/reac-aes67/conffiles
/etc/config/reac-aes67
endef

# No PKG_SOURCE: the C sources are vendored under ./src alongside this recipe
# (populated from the repo's src/ at build time). Stage them into PKG_BUILD_DIR.
define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)/src
	$(CP) ./src/*.c ./src/*.h $(PKG_BUILD_DIR)/src/
endef

# -DHAVE_UBUS enables the real libubus stats binding (ubus_stats.c); without it
# the daemon builds the no-op stub and the -lubus link would be unused.
define Build/Compile
	$(TARGET_CC) $(TARGET_CFLAGS) -std=c11 -O2 -DHAVE_UBUS \
		-I$(PKG_BUILD_DIR)/src -I$(STAGING_DIR)/usr/include \
		-o $(PKG_BUILD_DIR)/reac-aes67 \
		$(PKG_BUILD_DIR)/src/main.c \
		$(PKG_BUILD_DIR)/src/reac_decode.c \
		$(PKG_BUILD_DIR)/src/media_clock.c \
		$(PKG_BUILD_DIR)/src/pcap_source.c \
		$(PKG_BUILD_DIR)/src/rtp_l24.c \
		$(PKG_BUILD_DIR)/src/plc.c \
		$(PKG_BUILD_DIR)/src/pipeline.c \
		$(PKG_BUILD_DIR)/src/aes67_send.c \
		$(PKG_BUILD_DIR)/src/reac_capture.c \
		$(PKG_BUILD_DIR)/src/sdp.c \
		$(PKG_BUILD_DIR)/src/sap.c \
		$(PKG_BUILD_DIR)/src/ubus_stats.c \
		$(PKG_BUILD_DIR)/src/tone.c \
		$(PKG_BUILD_DIR)/src/ptp_clock.c \
		$(PKG_BUILD_DIR)/src/packetizer.c \
		-L$(STAGING_DIR)/usr/lib -lreac -lubus -lblobmsg_json -lubox -lm
endef

define Package/reac-aes67/install
	$(INSTALL_DIR) $(1)/usr/bin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/reac-aes67 $(1)/usr/bin/reac-aes67
	$(INSTALL_DIR) $(1)/etc/config
	$(INSTALL_CONF) ./files/reac-aes67.config $(1)/etc/config/reac-aes67
	$(INSTALL_DIR) $(1)/etc/init.d
	$(INSTALL_BIN) ./files/reac-aes67.init $(1)/etc/init.d/reac-aes67
	$(INSTALL_DIR) $(1)/etc/capabilities
	$(INSTALL_CONF) ./files/capabilities-reac-aes67.json $(1)/etc/capabilities/reac-aes67.json
	$(INSTALL_DIR) $(1)/usr/share/man/man8
	gzip -9nc ./files/reac-aes67.8 > $(PKG_BUILD_DIR)/reac-aes67.8.gz
	$(INSTALL_DATA) $(PKG_BUILD_DIR)/reac-aes67.8.gz $(1)/usr/share/man/man8/reac-aes67.8.gz
endef

$(eval $(call BuildPackage,reac-aes67))
