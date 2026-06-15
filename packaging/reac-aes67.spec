# SPDX-License-Identifier: GPL-3.0-or-later
# reac-aes67 — Roland REAC -> AES67 bridge daemon, Fedora build (no ubus).
%global debug_package %{nil}
Name:           reac-aes67
Version:        0.3.0
Release:        1%{?dist}
Summary:        Roland REAC to AES67 (RTP-L24) bridge daemon

License:        GPL-3.0-or-later
URL:            https://github.com/FreeREAC/reac-aes67
Source0:        %{name}-%{version}.tar.gz

BuildRequires:  gcc
BuildRequires:  make
BuildRequires:  systemd-rpm-macros

%description
reac-aes67 captures a Roland REAC stream off the wire (raw AF_PACKET, EtherType
0x8819), decodes the 24-bit audio, and re-emits it as AES67 / RTP-L24 multicast
with optional SAP/SDP discovery (and a --profile dante interop mode). It is the
same bridge that ships as an OpenWrt apk; this Fedora build omits the OpenWrt
ubus stats interface (built without -DHAVE_UBUS). The source tarball vendors
libreac so the build is self-contained.

%prep
%autosetup -n %{name}-%{version}

%build
make LIBREAC=third_party/libreac

%install
install -Dm0755 build/reac-aes67 %{buildroot}%{_bindir}/reac-aes67
install -Dm0644 packaging/reac-aes67.service %{buildroot}%{_unitdir}/reac-aes67.service
install -Dm0644 packaging/reac-aes67.conf     %{buildroot}%{_sysconfdir}/reac-aes67/reac-aes67.conf

%files
%license LICENSE
%doc README.md
%{_bindir}/reac-aes67
%{_unitdir}/reac-aes67.service
%dir %{_sysconfdir}/reac-aes67
%config(noreplace) %{_sysconfdir}/reac-aes67/reac-aes67.conf

%post
%systemd_post reac-aes67.service
%preun
%systemd_preun reac-aes67.service
%postun
%systemd_postun_with_restart reac-aes67.service

%changelog
* Mon Jun 15 2026 Pau Aliagas <linuxnow@gmail.com> - 0.3.0-1
- Fedora package: standalone REAC->AES67 bridge daemon (no ubus).
