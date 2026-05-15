SUMMARY = "FluxIPC shared memory IPC library"
DESCRIPTION = "High-performance shared memory IPC framework"
HOMEPAGE = "https://example.com/fluxipc"
SECTION = "libs"

LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://LICENSE;md5=<fill_me>"

PV = "1.0"

SRC_URI = "file://."

S = "${WORKDIR}"

inherit pkgconfig

# readline 依赖
DEPENDS += "readline"

# 传递 Yocto toolchain
EXTRA_OEMAKE = "\
    CC='${CC}' \
    CFLAGS='${CFLAGS}' \
    LDFLAGS='${LDFLAGS}' \
"

# ─────────────────────────────────────────────
# Build
# ─────────────────────────────────────────────

do_compile() {
    oe_runmake lib
}

# ─────────────────────────────────────────────
# Install
# ─────────────────────────────────────────────

do_install() {
    oe_runmake install \
        DESTDIR=${D} \
        PREFIX=${prefix} \
        LIBDIR=${libdir} \
        INCLUDEDIR=${includedir}/fluxipc
}

# ─────────────────────────────────────────────
# Package split
# ─────────────────────────────────────────────

# runtime package
FILES:${PN} += "\
    ${libdir}/libfluxipc.so.* \
"

# development package
FILES:${PN}-dev += "\
    ${libdir}/libfluxipc.so \
    ${includedir}/fluxipc \
"

# debug symbols
FILES:${PN}-dbg += "\
    ${libdir}/.debug \
"

# ─────────────────────────────────────────────
# QA
# ─────────────────────────────────────────────

# 避免开发链接检查误报
INSANE_SKIP:${PN} += "dev-so"

# readline runtime dependency
RDEPENDS:${PN} += "readline"
