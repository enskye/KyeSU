#!/bin/sh
# Add clang $1 from apt.llvm.org to a DDK image and shim it ahead of the image's
# own toolchain in /opt/tc/bin. Shared by the Containerfile (local builds) and
# ddk-lkm.yml (CI) so the two cannot drift apart.
set -eu
V="${1:?llvm version}"

apt-get update -qq
apt-get install -y -qq --no-install-recommends wget gnupg ca-certificates
. /etc/os-release
wget -qO /usr/share/keyrings/llvm.asc https://apt.llvm.org/llvm-snapshot.gpg.key
echo "deb [signed-by=/usr/share/keyrings/llvm.asc] http://apt.llvm.org/$VERSION_CODENAME/ llvm-toolchain-$VERSION_CODENAME-$V main" \
	> /etc/apt/sources.list.d/llvm.list
apt-get update -qq
apt-get install -y -qq --no-install-recommends "clang-$V" "lld-$V" "llvm-$V"

# kbuild here picks the compiler off PATH and ignores CC= on the command line
mkdir -p /opt/tc/bin
for t in clang ld.lld llvm-ar llvm-nm llvm-objcopy llvm-objdump llvm-readelf llvm-strip llvm-size; do
	ln -sf "/usr/bin/$t-$V" "/opt/tc/bin/$t"
done
apt-get clean
rm -rf /var/lib/apt/lists/*
