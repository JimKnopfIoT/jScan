Name:       jscan

%define _buildhost reproducible-builder

%{!?qtc_qmake5:%define qtc_qmake5 %qmake5}
%{!?qtc_make:%define qtc_make make}
%{?qtc_builddir:%define _builddir %qtc_builddir}
Summary:    Document scanning for Canon P-215II, Epson V600 and eSCL network scanners
Version:    0.1.0
Release:    1
Group:      Qt/Qt
License:    MIT and GPLv2+ and Apache-2.0 and BSD-2-Clause
Source0:    %{name}-%{version}.tar.bz2
Vendor:     reproducible-builder
Packager:   jscan contributors
Requires:   sailfishsilica-qt5 >= 0.10.9
Requires:   libusb1
BuildRequires:  pkgconfig(sailfishapp) >= 1.0.2
BuildRequires:  pkgconfig(Qt5Core)
BuildRequires:  pkgconfig(Qt5Qml)
BuildRequires:  pkgconfig(Qt5Quick)
BuildRequires:  pkgconfig(Qt5Network)
BuildRequires:  pkgconfig(Qt5Concurrent)
BuildRequires:  libusb1-devel
BuildRequires:  desktop-file-utils
BuildRequires:  qt5-qttools-linguist

%description
Scans documents from a Canon imageFORMULA P-215II on USB-OTG (SANE canon_dr,
statically linked), from an Epson Perfection V600 Photo (own open driver; the
Epson firmware file has to be copied to the phone separately) and from
eSCL/AirScan network scanners. Pages can be
adjusted after scanning (contrast, background, deskew, colour) and are saved
as PDF (optionally searchable via bundled Tesseract OCR) or PNG.

%prep
%setup -q -n %{name}-%{version}

%build
%qtc_qmake5
%qtc_make %{?_smp_mflags}

%install
rm -rf %{buildroot}
%qmake5_install

desktop-file-install --delete-original       \
  --dir %{buildroot}%{_datadir}/applications             \
   %{buildroot}%{_datadir}/applications/*.desktop

%post
udevadm control --reload-rules >/dev/null 2>&1 || :
udevadm trigger --subsystem-match=usb --attr-match=idVendor=1083 >/dev/null 2>&1 || :
udevadm trigger --subsystem-match=usb --attr-match=idVendor=04b8 >/dev/null 2>&1 || :
systemctl restart sailjaild >/dev/null 2>&1 || :

%files
%defattr(644,root,root,755)
%attr(755,root,root) %{_bindir}/%{name}
%dir %{_datadir}/%{name}
%dir %{_datadir}/%{name}/bin
%attr(755,root,root) %{_datadir}/%{name}/bin/tesseract
%{_datadir}/%{name}/qml
%{_datadir}/%{name}/sane
%{_datadir}/%{name}/tessdata
%{_datadir}/%{name}/translations
%{_datadir}/%{name}/licenses
%{_datadir}/applications/%{name}.desktop
%{_datadir}/icons/hicolor/*/apps/%{name}.png
/lib/udev/rules.d/65-jscan-canon.rules
/lib/udev/rules.d/65-jscan-epson-v600.rules
