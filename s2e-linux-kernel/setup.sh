#! /bin/sh

set -e

wget https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git/snapshot/linux-4.9.3.tar.gz
tar -xvf linux-4.9.3.tar.gz
cd linux-4.9.3
patch -p 1 -i ../patch-4.9.3
patch -p 1 -i ../ilog2_NaN.patch
cp -r ../../../s2e-linux-kernel/linux-4.9.3/kernel/s2e kernel/
cp -r ../../../s2e-linux-kernel/include ../
cp ../config_4.9.3 .config
rm ../linux-4.9.3.tar.gz
