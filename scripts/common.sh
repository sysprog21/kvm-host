# Package information
readonly LINUX_VER=5.14.3
readonly BUSYBOX_VER=1.34.0

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
    DL=$(eval "echo $`eval "echo ${PKG}_DL"`")
    ARCHIVE=$(eval "echo $`eval "echo ${PKG}_ARCHIVE"`")
    wget -c ${DL} -O ${OUT}/${ARCHIVE}
}
