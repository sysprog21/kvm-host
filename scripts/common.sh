# Package information
readonly LINUX_VER=5.14.4
readonly BUSYBOX_VER=1.34.0

# SHA Checksums (default: SHA-1)
# TODO: support multiple versions
readonly LINUX_CHECKSUM=72f09f2e84bb36928bcc9285cf2ef9ef851a6089
readonly BUSYBOX_CHECKSUM=68c63ab87768e9f0c16ffec79935e488ecf2fe58

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
