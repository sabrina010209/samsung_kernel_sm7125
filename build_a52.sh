#!/bin/bash

export ARCH=arm64
export KBUILD_BUILD_HOST="ubuntu"
export KBUILD_BUILD_USER="seb"

make O=out ARCH=arm64 vendor/a52q_eur_open_defconfig


PATH="${PWD}/toolchains/gcc_aarch64/bin:${PWD}/toolchains/gcc_arm/bin:${PATH}:${PWD}/toolchains/clang/bin:${PATH}"
ccache make O=out ARCH=arm64 CC=clang CROSS_COMPILE=aarch64-linux-android- CROSS_COMPILE_ARM32=arm-linux-androideabi- CLANG_TRIPLE=aarch64-linux-gnu-
