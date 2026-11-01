# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 KC2RGW Chris

# Packages with CPack, in a build configured with -DCMAKE_INSTALL_PREFIX=/usr:
# `cpack -G DEB` (Ubuntu): patchprime_<version>-<release>_<arch>.deb, and
# `cpack -G RPM` (Fedora): patchprime-<version>-<release>.<dist>.<arch>.rpm.
# Library dependencies come from dpkg-shlibdeps / rpm's automatic ones; the
# QML modules the GUI loads at run time, and PipeWire itself, are listed by
# hand (their package names differ per distribution). Arch Linux has its
# own PKGBUILD in packaging/arch.

set(CPACK_GENERATOR "DEB")
set(CPACK_PACKAGE_NAME "patchprime")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_VENDOR "KC2RGW Chris")
set(CPACK_PACKAGE_CONTACT "KC2RGW Chris <2788495+kingsqueak@users.noreply.github.com>")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${PROJECT_DESCRIPTION}")
set(CPACK_PACKAGE_DESCRIPTION
    "Patch Prime turns your computer's audio into a small mixing console: every \
input, app and output gets a channel strip with a fader, mute and meters; \
mix buses and virtual devices route sound between them; scenes save the mix; \
and built-in effects (EQ, compressor, gate, ducker, noise suppression, \
leveler, limiter, reverb, delay) process any strip. The engine runs as a \
systemd user service (patchprimed); the window is a Kirigami app.")
set(CPACK_RESOURCE_FILE_LICENSE "${PROJECT_SOURCE_DIR}/LICENSE")
# Stripped (the build keeps debug info), with directories 755 whatever the
# builder's umask.
set(CPACK_STRIP_FILES ON)
set(CPACK_INSTALL_DEFAULT_DIRECTORY_PERMISSIONS
    OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)

set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
set(CPACK_DEBIAN_PACKAGE_RELEASE "1")
set(CPACK_DEBIAN_PACKAGE_SECTION "sound")
set(CPACK_DEBIAN_PACKAGE_HOMEPAGE "https://github.com/kingsqueak/patch-prime")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_DEBIAN_PACKAGE_DEPENDS
    "pipewire (>= 1.6), wireplumber, qml6-module-org-kde-kirigami, qml6-module-org-kde-kquickcontrols, \
qml6-module-org-kde-desktop, qml6-module-qtquick-controls, \
qml6-module-qtquick-layouts, qml6-module-qtquick-window, \
qml6-module-qtquick-templates, qml6-module-qtqml-workerscript, qml6-module-qtcore")
# Starts the engine at every user's login (it's also D-Bus activated when
# the window opens).
set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA
    "${PROJECT_SOURCE_DIR}/packaging/debian/postinst;${PROJECT_SOURCE_DIR}/packaging/debian/prerm")
set(CPACK_DEBIAN_PACKAGE_CONTROL_STRICT_PERMISSION ON)

# Fedora
set(CPACK_RPM_PACKAGE_RELEASE "1")
set(CPACK_RPM_PACKAGE_RELEASE_DIST ON) # .fc44
set(CPACK_RPM_FILE_NAME RPM-DEFAULT)
# Not relocatable: the unit, D-Bus and .desktop files hold /usr/bin paths.
set(CPACK_PACKAGE_RELOCATABLE OFF)
set(CPACK_RPM_PACKAGE_RELOCATABLE OFF)
set(CPACK_RPM_PACKAGE_LICENSE "GPL-3.0-or-later")
set(CPACK_RPM_PACKAGE_GROUP "Applications/Multimedia")
set(CPACK_RPM_PACKAGE_URL "https://github.com/kingsqueak/patch-prime")
set(CPACK_RPM_PACKAGE_DESCRIPTION "${CPACK_PACKAGE_DESCRIPTION}")
set(CPACK_RPM_PACKAGE_REQUIRES
    "pipewire >= 1.6, wireplumber, kf6-kirigami, kf6-kdeclarative, kf6-qqc2-desktop-style, qt6-qtdeclarative, lilv-libs, alsa-lib")
set(CPACK_RPM_POST_INSTALL_SCRIPT_FILE "${PROJECT_SOURCE_DIR}/packaging/rpm/postinstall")
set(CPACK_RPM_PRE_UNINSTALL_SCRIPT_FILE "${PROJECT_SOURCE_DIR}/packaging/rpm/preuninstall")
# Directories other packages own.
list(APPEND CPACK_RPM_EXCLUDE_FROM_AUTO_FILELIST_ADDITION
     /usr/share/applications /usr/share/dbus-1 /usr/share/dbus-1/services
     /usr/share/systemd /usr/share/systemd/user /usr/lib/systemd /usr/lib/systemd/user)

include(CPack)
