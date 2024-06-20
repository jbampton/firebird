#MACTOOLS=/usr/home/firebird/Mac/mactools

export PATH=${MACTOOLS}/usr/bin:$PATH
export LD_LIBRARY_PATH=${MACTOOLS}/usr/lib:$LD_LIBRARY_PATH

CROSS_PREFIX=${MACTOOLS}/usr/bin/arm-apple-darwin11-

export CXX=${CROSS_PREFIX}clang++
export CC=${CROSS_PREFIX}clang
export AR=${CROSS_PREFIX}ar

#AS=${CROSS_PREFIX}as

export LD=${CROSS_PREFIX}ld

#NM=${CROSS_PREFIX}nm
#OBJCOPY=${CROSS_PREFIX}objcopy
#OBJDUMP=${CROSS_PREFIX}objdump

export RANLIB=${CROSS_PREFIX}ranlib

#STRIP=${CROSS_PREFIX}strip
