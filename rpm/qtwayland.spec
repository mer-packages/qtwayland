%define _qtmodule_snapshot_version 0.0-git855.e5601d283c
Name:       qt5-qtwayland-wayland_egl
Summary:    Qt Wayland compositor, wayland_egl variant
Version:    0.0git855.e5601d283c
Release:    1%{?dist}
Group:      Qt/Qt
License:    LGPLv2.1 with exception or GPLv3
URL:        http://qt.nokia.com
Source0:    %{name}-%{version}.tar.bz2
Source100:	precheckin.sh
BuildRequires:  pkgconfig(Qt5Core)
BuildRequires:  pkgconfig(Qt5Gui)
BuildRequires:  pkgconfig(Qt5Widgets)
BuildRequires:  pkgconfig(Qt5OpenGL)
BuildRequires:  pkgconfig(Qt5PlatformSupport)
BuildRequires:  pkgconfig(Qt5Qml)
BuildRequires:  pkgconfig(Qt5Declarative)
BuildRequires:  pkgconfig(Qt5Quick)
BuildRequires:  pkgconfig(Qt5V8)
BuildRequires:  pkgconfig(Qt5DBus)

BuildRequires:  pkgconfig(wayland-client)
%if "%{name}" == "qt5-qtwayland-wayland_egl"
BuildRequires:  pkgconfig(wayland-egl)
%endif

BuildRequires:  libxkbcommon-devel
BuildRequires:  pkgconfig(glib-2.0)
BuildRequires:  libffi-devel
BuildRequires:  fdupes

Requires:       xkeyboard-config

%description
Qt is a cross-platform application and UI framework. Using Qt, you can
write web-enabled applications once and deploy them across desktop,
mobile and embedded systems without rewriting the source code.
.
This package contains the Qt wayland compositor for wayland_egl

%package devel
Summary:        Qt Wayland compositor - development files
Group:          Qt/Qt
Requires:       %{name} = %{version}-%{release}

%description devel
Qt is a cross-platform application and UI framework. Using Qt, you can
write web-enabled applications once and deploy them across desktop,
mobile and embedded systems without rewriting the source code.
.
This package contains the Qt wayland compositor development files for wayland_egl

%package examples
Summary:        Qt Wayland compositor - examples
Group:          Qt/Qt
Requires:       %{name} = %{version}-%{release}

%description examples
Qt is a cross-platform application and UI framework. Using Qt, you can
write web-enabled applications once and deploy them across desktop,
mobile and embedded systems without rewriting the source code.
.
This package contains the Qt wayland compositor examples for wayland_egl

#### Build section

%prep
%setup -q -n %{name}-%{version}/qtwayland

%build
export QTDIR=/usr/share/qt5
export QT_WAYLAND_GL_CONFIG=wayland_egl
touch .git
%qmake5 "QT_BUILD_PARTS += examples" "CONFIG += wayland-compositor" 

make %{?_smp_mflags}

%install
rm -rf %{buildroot}
%qmake_install
# Fix wrong path in pkgconfig files
find %{buildroot}%{_libdir}/pkgconfig -type f -name '*.pc' \
-exec perl -pi -e "s, -L%{_builddir}/?\S+,,g" {} \;
# Fix wrong path in prl files
find %{buildroot}%{_libdir} -type f -name '*.prl' \
-exec sed -i -e "/^QMAKE_PRL_BUILD_DIR/d;s/\(QMAKE_PRL_LIBS =\).*/\1/" {} \;

# We don't need qt5/Qt/
rm -rf %{buildroot}/%{_includedir}/qt5/Qt


%fdupes %{buildroot}/%{_includedir}


#### Pre/Post section

%post -p /sbin/ldconfig
%postun -p /sbin/ldconfig


#### File section

%files
%defattr(-,root,root,-)
%{_libdir}/libQt5Compositor.so.5
%{_libdir}/libQt5Compositor.so.5.*

%if "%{name}" == "qt5-qtwayland-wayland_egl"
%{_libdir}/qt5/plugins/platforms/libqwayland-egl.so
%{_libdir}/qt5/plugins/waylandcompositors/libwayland-egl.so
%endif

%if "%{name}" == "qt5-qtwayland-xcomposite_egl"
%{_libdir}/qt5/plugins/platforms/libqwayland-xcomposite-egl.so
%{_libdir}/qt5/plugins/waylandcompositors/libxcomposite-egl.so
%endif

%if "%{name}" == "qt5-qtwayland-nogl"
%{_libdir}/qt5/plugins/platforms/libqwayland-nogl.so
%endif

%files devel
%defattr(-,root,root,-)
%{_libdir}/libQt5Compositor.so
%{_includedir}/qt5/*
%{_libdir}/libQt5Compositor.la
%{_libdir}/libQt5Compositor.prl
%{_libdir}/pkgconfig/Qt5Compositor.pc
%{_libdir}/cmake/Qt5Compositor/*
%{_datadir}/qt5/mkspecs/modules/qt_lib_compositor.pri
%{_libdir}/qt5/bin/qtwaylandscanner

%files examples
%defattr(-,root,root,-)
%{_libdir}/qt5/examples/qtwayland/

#### No changelog section, separate $pkg.changes contains the history
