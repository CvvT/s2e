#! /bin/bash

set -e

VERSION=$1
if [[ ! -f "patch-${VERSION}" ]]; then
    echo "Do not support version ${VERSION}" && exit 1
fi

# FIXME: 4.14.0
wget https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git/snapshot/linux-${VERSION}.tar.gz
tar -xzf linux-${VERSION}.tar.gz
cd linux-${VERSION}
patch -p 1 -i ../patch-${VERSION}
patch -N -p 1 -i ../ilog2_NaN.patch || true
cp -r ../../../s2e-linux-kernel/linux-4.9.3/kernel/s2e kernel/
cp -r ../../../s2e-linux-kernel/include ../
cp ../config_${VERSION} .config
rm ../linux-${VERSION}.tar.gz
