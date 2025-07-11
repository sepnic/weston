#!/bin/bash
#
# Builds the dependencies required for any OS/architecture combination. See
# .gitlab-ci.yml for more information. This script is called from an
# OS-specific build scripts like debian-install.sh.

set -o xtrace -o errexit

# Set concurrency to an appropriate level for our shared runners, falling back
# to the conservative default form before we had this variable.
export MAKEFLAGS="-j${FDO_CI_CONCURRENT:-4}"
export NINJAFLAGS="-j${FDO_CI_CONCURRENT:-4}"

# When calling pip in newer versions, we're required to pass
# --break-system-packages so it knows that we did really want to call pip and
# aren't just doing it by accident.
PIP_ARGS="--user"
case "$FDO_DISTRIBUTION_VERSION" in
  bullseye)
    ;;
  *)
    PIP_ARGS="$PIP_ARGS --break-system-packages"
    ;;
esac

# Build and install Meson. Generally we want to keep this in sync with what
# we require inside meson.build.
pip3 install $PIP_ARGS git+https://github.com/mesonbuild/meson.git@1.3.2
export PATH=$HOME/.local/bin:$PATH

# Our docs are built using Sphinx (top-level organisation and final HTML/CSS
# generation), Doxygen (parse structures/functions/comments from source code),
# Breathe (a bridge between Doxygen and Sphinx), and we use the Read the Docs
# theme for the final presentation.
pip3 install $PIP_ARGS sphinx==4.2.0
pip3 install $PIP_ARGS sphinxcontrib-applehelp==1.0.4
pip3 install $PIP_ARGS sphinxcontrib-devhelp==1.0.2
pip3 install $PIP_ARGS sphinxcontrib-htmlhelp==2.0.0
pip3 install $PIP_ARGS sphinxcontrib-jsmath==1.0.1
pip3 install $PIP_ARGS sphinxcontrib-qthelp==1.0.3
pip3 install $PIP_ARGS sphinxcontrib-serializinghtml==1.1.5
pip3 install $PIP_ARGS breathe==4.31.0
pip3 install $PIP_ARGS sphinx_rtd_theme==1.0.0

# Build a Linux kernel for use in testing. We enable the VKMS module so we can
# predictably test the DRM backend in the absence of real hardware. We lock the
# version here so we see predictable results.
#
# To run this we use virtme, a QEMU wrapper: https://github.com/amluto/virtme
#
# virtme makes our lives easier by abstracting handling of the console,
# filesystem, etc, so we can pretend that the VM we execute in is actually
# just a regular container.
#
# The reason why we are using a fork here is that it adds a patch to have the
# --script-dir command line option. With that we can run scripts that are in a
# certain folder when virtme starts, which is necessary in our use case.
#
# The upstream also has some commands that could help us to reach the same
# results: --script-sh and --script-exec. Unfornutately they are not completely
# implemented yet, so we had some trouble to use them and it was becoming
# hackery.
#
# The fork pulls in this support from the original GitHub PR, rebased on top of
# a newer upstream version which fixes AArch64 support.
if [[ -n "$KERNEL_DEFCONFIG" ]]; then
	# 6.3 is (still) used as >= 6.5 drm-writeback test will timeout
	git clone --depth=1 --branch=v6.3 https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git linux
	cd linux

	if [[ "${BUILD_ARCH}" = "x86-64" ]]; then
		LINUX_ARCH=x86
	elif [[ "$BUILD_ARCH" = "aarch64" ]]; then
		LINUX_ARCH=arm64
	else
		echo "Invalid or missing \$BUILD_ARCH"
		exit 1
	fi

	if [[ -z "${KERNEL_DEFCONFIG}" ]]; then
		echo "Invalid or missing \$KERNEL_DEFCONFIG"
		exit
	fi
	if [[ -z "${KERNEL_IMAGE}" ]]; then
		echo "Invalid or missing \$KERNEL_IMAGE"
		exit
	fi

	make ARCH=${LINUX_ARCH} ${KERNEL_DEFCONFIG}
	make ARCH=${LINUX_ARCH} kvm_guest.config
	./scripts/config \
		--enable CONFIG_DRM \
		--enable CONFIG_DRM_KMS_HELPER \
		--enable CONFIG_DRM_KMS_FB_HELPER \
		--enable CONFIG_DRM_VKMS \
		--enable CONFIG_UDMABUF
	make ARCH=${LINUX_ARCH} oldconfig
	make ARCH=${LINUX_ARCH}

	cd ..
	mkdir /weston-virtme
	mv linux/arch/${LINUX_ARCH}/boot/${KERNEL_IMAGE} /weston-virtme/
	mv linux/.config /weston-virtme/.config
	rm -rf linux

	git clone --depth=1 --branch=v1.25 --recurse-submodules https://github.com/arighi/virtme-ng.git virtme
	cd virtme
	./setup.py install
	cd ..
fi

# Build and install Wayland; keep this version in sync with our dependency
# in meson.build.
git clone --branch 1.22.0 --depth=1 https://gitlab.freedesktop.org/wayland/wayland
cd wayland
git show -s HEAD
meson setup build --wrap-mode=nofallback -Ddocumentation=false
ninja ${NINJAFLAGS} -C build install
cd ..
rm -rf wayland

# Keep this version in sync with our dependency in meson.build. If you wish to
# raise a MR against custom protocol, please change this reference to clone
# your relevant tree, and make sure you bump $FDO_DISTRIBUTION_TAG.
git clone --branch 1.44 --depth=1 https://gitlab.freedesktop.org/wayland/wayland-protocols
cd wayland-protocols
git show -s HEAD
meson setup build --wrap-mode=nofallback -Dtests=false
ninja ${NINJAFLAGS} -C build install
cd ..
rm -rf wayland-protocols

# Build and install our own version of libdrm. Debian 11 (bullseye) provides
# libdrm 2.4.104 which doesn't have the IN_FORMATS iterator api, and Mesa
# depends on 2.4.109 as well.
# Bump to 2.4.118 to include DRM_FORMAT_NV{15,20,30}
git clone --branch libdrm-2.4.118 --depth=1 https://gitlab.freedesktop.org/mesa/drm.git
cd drm
meson setup build --wrap-mode=nofallback -Dauto_features=disabled \
	-Dvc4=disabled -Dfreedreno=disabled -Detnaviv=disabled
ninja ${NINJAFLAGS} -C build install
cd ..
rm -rf drm

# Build and install our own version of Mesa. Debian provides a perfectly usable
# Mesa, however llvmpipe's rendering behaviour can change subtly over time.
# This doesn't work for our tests which expect pixel-precise reproduction, so
# we lock it to a set version for more predictability. If you need newer
# features from Mesa then bump this version and $FDO_DISTRIBUTION_TAG, however
# please be prepared for some of the tests to change output, which will need to
# be manually inspected for correctness.
git clone --single-branch --branch main https://gitlab.freedesktop.org/mesa/mesa.git
cd mesa
git checkout -b snapshot 7b68e1da91732b7d9bb9bf620cf8d4f63a48ea8c
meson setup build --wrap-mode=nofallback -Dauto_features=disabled \
	-Dgallium-drivers=llvmpipe -Dvulkan-drivers=swrast -Dvideo-codecs= \
	-Degl=enabled -Dgbm=enabled -Dgles2=enabled -Dllvm=enabled \
	-Dshared-glapi=enabled
ninja ${NINJAFLAGS} -C build install
cd ..
rm -rf mesa

# PipeWire is used for remoting support. Unlike our other dependencies its
# behaviour will be stable, however as a pre-1.0 project its API is not yet
# stable, so again we lock it to a fixed version.
#
# ... the version chosen is 0.3.32 with a small Clang-specific build fix.
git clone --single-branch --branch master https://gitlab.freedesktop.org/pipewire/pipewire.git pipewire-src
cd pipewire-src
git checkout -b snapshot bf112940d0bf8f526dd6229a619c1283835b49c2
meson setup build --wrap-mode=nofallback
ninja ${NINJAFLAGS} -C build install
cd ..
rm -rf pipewire-src

# seatd lets us avoid the pain of open-coding TTY assignment within Weston.
# We use this for our tests using the DRM backend.
git clone --depth=1 --branch 0.6.1 https://git.sr.ht/~kennylevinsen/seatd
cd seatd
meson setup build --wrap-mode=nofallback -Dauto_features=disabled \
	-Dlibseat-seatd=enabled -Dlibseat-logind=systemd -Dserver=enabled
ninja ${NINJAFLAGS} -C build install
cd ..
rm -rf seatd

# Build and install aml and neatvnc, which are required for the VNC backend
git clone --branch v0.3.0 --depth=1 https://github.com/any1/aml.git
cd aml
meson setup build --wrap-mode=nofallback
ninja ${NINJAFLAGS} -C build install
cd ..
rm -rf aml
git clone --branch v0.7.0 --depth=1 https://github.com/any1/neatvnc.git
cd neatvnc
meson setup build --wrap-mode=nofallback -Dauto_features=disabled
ninja ${NINJAFLAGS} -C build install
cd ..
rm -rf neatvnc

# Build and install libdisplay-info, used by drm-backend
git clone --branch 0.2.0 --depth=1 https://gitlab.freedesktop.org/emersion/libdisplay-info.git
cd libdisplay-info
meson setup build --wrap-mode=nofallback
ninja ${NINJAFLAGS} -C build install
cd ..
rm -rf libdisplay-info

# Build and install lcms2, which we use to support color-management.
git clone --branch master https://github.com/mm2/Little-CMS.git lcms2
cd lcms2
git checkout -b snapshot lcms2.16
meson setup build --wrap-mode=nofallback
ninja ${NINJAFLAGS} -C build install
cd ..
rm  -rf lcms2
