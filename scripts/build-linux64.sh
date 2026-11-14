#!/usr/bin/env bash
# Isolated, repeatable x86-64 build for Steam Runtime glibc 2.31.
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
cr_root="$PWD"
cr_deps="$cr_root/build-deps"
cr_jobs="${CR_BUILD_JOBS:-4}"
mkdir -p "$cr_deps/downloads" "$cr_deps/sysroot"
python3 - <<'PY'
import hashlib,json,os,pathlib,subprocess,urllib.request
root=pathlib.Path('build-deps').resolve(); sysroot=root/'sysroot'
for pkg in json.loads(pathlib.Path('scripts/linux64-packages.json').read_text()):
    p=root/'downloads'/pathlib.Path(pkg['Filename']).name
    if not p.exists(): urllib.request.urlretrieve(pkg.get('BaseURL','https://deb.debian.org/debian/')+pkg['Filename'],p)
    if hashlib.sha256(p.read_bytes()).hexdigest()!=pkg['SHA256']:
        raise SystemExit(f'Checksum mismatch: {p}')
    ar=subprocess.Popen(['ar','p',str(p),'data.tar.xz'],stdout=subprocess.PIPE)
    subprocess.run(['tar','-xJf','-','-C',str(sysroot)],stdin=ar.stdout,check=True)
    ar.stdout.close()
    if ar.wait(): raise SystemExit('ar failed')
# Debian's absolute loader links must stay inside this extracted sysroot.
for p in sysroot.rglob('*'):
    if p.is_symlink() and os.readlink(p).startswith('/'):
        target=str(sysroot)+os.readlink(p)
        p.unlink();p.symlink_to(os.path.relpath(target,p.parent))
sources=[
 ('https://www.openssl.org/source/openssl-3.5.4.tar.gz','openssl.tar.gz','967311f84955316969bdb1d8d4b983718ef42338639c621ec4c34fddef355e99'),
 ('https://curl.se/download/curl-8.23.0.tar.xz','curl.tar.xz','8dadd7d2536a314f982ad05a651b713f426bf7d02cc2c76cffb9796f2218e6c5')]
for url,name,sha in sources:
    p=root/'downloads'/name
    if not p.exists():urllib.request.urlretrieve(url,p)
    if hashlib.sha256(p.read_bytes()).hexdigest()!=sha:raise SystemExit(f'Checksum mismatch: {p}')
PY
cr_cc="clang --sysroot=$cr_deps/sysroot --gcc-toolchain=$cr_deps/sysroot/usr"
if [[ ! -f "$cr_deps/static/lib/libcrypto.a" ]]; then
    mkdir -p "$cr_deps/openssl-src"
    tar -xzf "$cr_deps/downloads/openssl.tar.gz" --strip-components=1 -C "$cr_deps/openssl-src"
    (
        cd "$cr_deps/openssl-src"
        CC="$cr_cc" ./Configure linux-x86_64 no-shared no-tests no-module -fPIC \
            --prefix="$cr_deps/static" --libdir=lib > ../openssl-configure.log 2>&1
        make -j"$cr_jobs" > ../openssl-build.log 2>&1
        make install_sw > ../openssl-install.log 2>&1
    )
fi
if [[ ! -f "$cr_deps/static/lib/libcurl.a" ]]; then
    mkdir -p "$cr_deps/curl-src"
    tar -xJf "$cr_deps/downloads/curl.tar.xz" --strip-components=1 -C "$cr_deps/curl-src"
    (
        cd "$cr_deps/curl-src"
        CC="$cr_cc" CPPFLAGS="-I$cr_deps/static/include" \
        LDFLAGS="-L$cr_deps/static/lib -Wl,-rpath-link,$cr_deps/sysroot/lib/x86_64-linux-gnu" \
        LIBS='-ldl -lpthread' PKG_CONFIG=/bin/false \
        ./configure --prefix="$cr_deps/static" --disable-shared --enable-static --with-pic \
            --disable-docs --disable-manual --with-ca-bundle=/etc/ssl/certs/ca-certificates.crt --with-openssl="$cr_deps/static" \
            --without-zlib --without-libpsl --without-zstd --without-brotli --without-libidn2 \
            --without-nghttp2 --without-libssh2 --without-librtmp --without-ngtcp2 --without-quiche \
            --disable-ldap --disable-ldaps --disable-ftp --disable-file --disable-dict \
            --disable-telnet --disable-tftp --disable-imap --disable-pop3 --disable-smtp \
            --disable-smb --disable-gopher --disable-mqtt > ../curl-configure.log 2>&1
        make -j"$cr_jobs" > ../curl-build.log 2>&1
        make install > ../curl-install.log 2>&1
    )
fi
cmake -S . -B build-steamrt64 -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$cr_root/cmake/linux64-steamrt.cmake" \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo -DLINUX_32BIT=OFF \
    -DCR_DEPS_PREFIX="$cr_deps/static"
cmake --build build-steamrt64 -j"$cr_jobs"
ctest --test-dir build-steamrt64 --output-on-failure
python3 scripts/check-linux64-abi.py build-steamrt64/cloud_redirect.so
