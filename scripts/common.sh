# Package information
readonly LINUX_VER=5.18.8
readonly BUSYBOX_VER=1.35.0

# SHA Checksums (default: SHA-1)
# TODO: support multiple versions
readonly LINUX_CHECKSUM=8db5e3c3bc63a66fba5cdac53c125252dfbf3b82
readonly BUSYBOX_CHECKSUM=36a1766206c8148bc06aca4e1f134016d40912d0

# General rules
TOP=$(cd "$(dirname "$0")" ; cd .. ; pwd)
OUT=${TOP}/build
mkdir -p ${OUT}
CONF=${TOP}/configs
FILE=${TOP}/target

# Helpers

# parallel build
PARALLEL="-j `nproc`"

# get build directory of specified package name
function buildpath()
{
    PKG=${1^^}
    VER=$(eval "echo $`eval "echo ${PKG}_VER"`")
    P=${OUT}/${1}-${VER}
    if [ ! -d ${P} ]; then
        echo "ERROR: directory ${P} does not exist!"
        exit 1
    fi
    echo ${P}
}

# download specific package
function download()
{
    PKG=${1^^}
    VER=$(eval "echo $`eval "echo ${PKG}_VER"`")
    DL=$(eval "echo $`eval "echo ${PKG}_DL"`")
    CHECKSUM=$(eval "echo $`eval "echo ${PKG}_CHECKSUM"`")
    ARCHIVE=$(basename ${DL})
    echo "Downloading ${1} version ${VER} ..."
    wget -c ${DL} -O ${OUT}/${ARCHIVE} || exit 1
    echo "${CHECKSUM}  ${OUT}/${ARCHIVE}" | shasum -c || exit 1
}

function extract()
{
    PKG=${1^^}
    DL=$(eval "echo $`eval "echo ${PKG}_DL"`")
    ARCHIVE=$(basename ${DL})
    echo "Extracting ${ARCHIVE} ..."
    tar -xf ${OUT}/${ARCHIVE} -C ${OUT} || exit 1
}
