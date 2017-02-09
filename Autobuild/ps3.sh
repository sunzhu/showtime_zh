TOOLCHAIN_URL=https://movian.tv/static/ps3dev.tar.gz
TOOLCHAIN="${WORKINGDIR}/ps3dev"

cleanup() {
    echo "Cleaning up"
    exit 1
}

export PS3DEV=${TOOLCHAIN}
export PSL1GHT=${TOOLCHAIN}/psl1ght
export PATH=$PATH:$PS3DEV/bin:$PS3DEV/host/ppu/bin:$PS3DEV/host/spu/bin
export PATH=$PATH:$PSL1GHT/host/bin

echo "Toolchain from: '${TOOLCHAIN_URL}' Local install in: ${TOOLCHAIN}"
if [ -d $TOOLCHAIN ]; then
    echo "Toolchain seems to exist"
else
    set +e
    trap cleanup SIGINT
    (
	set -eu
	cd ${WORKINGDIR}
	curl -L "${TOOLCHAIN_URL}" | tar xfz -
    )

    STATUS=$?
    if [ $STATUS -ne 0 ]; then
	echo "Unable to stage toolchain"
	cleanup
    fi
    trap SIGINT
    set -e
fi

which ccache >/dev/null
if [ $? -eq 0 ]; then
    echo "Using ccache"
    ccache -s
    USE_CCACHE="--ccache"
else
    USE_CCACHE=""
fi

./configure.ps3 --build=${TARGET} \
    ${RELEASE} \
    --cleanbuild \
    ${USE_CCACHE} \
    --downloadcache="${WORKINGDIR}/downloadcache"


make ${JARGS} BUILD=${TARGET} pkg self
artifact build.${TARGET}/showtime.self self application/octect-stream showtime.self
artifact build.${TARGET}/showtime.pkg pkg application/octect-stream showtime.pkg
artifact build.${TARGET}/showtime_geohot.pkg pkg application/octect-stream showtime-gh.pkg
