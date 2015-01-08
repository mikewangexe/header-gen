export LINUX_DIR=/home/chyz/research/linux/linux-stable
export REMOVE_INLINE_DEFINITIONS=

export ARCH=x86
export BOARD=
export TOOLCHAIN_PREFIX=
export PLATFORM_CC_FLAGS=

export TOP="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
export CLANG=$TOP/bin/clang
export LD_LIBRARY_PATH=$TOP/lib:$TOP/lib64:$LD_LIBRARY_PATH

if [ -d $LINUX_DIR ]; then
    pushd $LINUX_DIR > /dev/null
    if [ ! -f .config ]; then
	make defconfig
	make init
    fi
    popd > /dev/null
fi
