# FIDO2 security keys: libfido2 with libcbor and OpenSSL's libcrypto, built from the pinned sources with their own
# build systems and linked statically. Defines the target room237_fido.
include(ExternalProject)

foreach(_dir libfido2 libcbor openssl zlib)
  if(NOT EXISTS "${_third}/${_dir}")
    message(FATAL_ERROR "${_third}/${_dir} is missing; run qt/packaging/ci/fetch_deps.py core-sources")
  endif()
endforeach()
# libfido2 finds its dependencies through pkg-config outside MSVC; OpenSSL configures with Perl and builds with make.
find_program(ROOM237_PKG_CONFIG NAMES pkg-config pkgconf REQUIRED)
find_program(ROOM237_PERL perl REQUIRED)
find_program(ROOM237_MAKE NAMES gmake make REQUIRED)

# A changed pin or build recipe builds into new directories: ExternalProject does not notice replaced sources, and
# OpenSSL's makefiles do not rebuild objects whose flags changed.
set(_lock "${CMAKE_CURRENT_LIST_DIR}/../../packaging/ci/deps.lock.json")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_lock}" "${CMAKE_CURRENT_LIST_FILE}")
file(SHA256 "${_lock}" _lock_hash)
file(SHA256 "${CMAKE_CURRENT_LIST_FILE}" _recipe_hash)
string(SHA256 _lock_hash "${_lock_hash}${_recipe_hash}")
string(SUBSTRING "${_lock_hash}" 0 12 _lock_hash)
set(_fido_prefix "${CMAKE_BINARY_DIR}/fido-${_lock_hash}")
set(_fido_lib "${_fido_prefix}/lib")
# The libraries are always optimized: Argon2id is not the only slow path in a debug build.
set(_fido_common
  -G "${CMAKE_GENERATOR}"
  -DCMAKE_BUILD_TYPE=Release
  "-DCMAKE_INSTALL_PREFIX=${_fido_prefix}"
  -DCMAKE_INSTALL_LIBDIR=lib
  -DCMAKE_INSTALL_INCLUDEDIR=include
  -DBUILD_SHARED_LIBS=OFF
  -DCMAKE_POSITION_INDEPENDENT_CODE=ON
  -DCMAKE_C_VISIBILITY_PRESET=hidden
  "-DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}"
  "-DCMAKE_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}")
if(CMAKE_TOOLCHAIN_FILE)
  list(APPEND _fido_common "-DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}")
endif()
if(APPLE)
  string(REPLACE ";" "|" _archs "${CMAKE_OSX_ARCHITECTURES}")
  list(APPEND _fido_common "-DCMAKE_OSX_ARCHITECTURES=${_archs}" "-DCMAKE_OSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET}" "-DCMAKE_OSX_SYSROOT=${CMAKE_OSX_SYSROOT}")
endif()

# pkg-config sees the libraries built here first, then the system's (libudev on Linux).
set(_pc "${_fido_lib}/pkgconfig")
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  string(APPEND _pc ":/usr/lib/${CMAKE_LIBRARY_ARCHITECTURE}/pkgconfig:/usr/lib/pkgconfig:/usr/share/pkgconfig:/usr/lib64/pkgconfig")
endif()
set(_pkg_env "${CMAKE_COMMAND}" -E env "PKG_CONFIG_LIBDIR=${_pc}" PKG_CONFIG_PATH= "PKG_CONFIG=${ROOM237_PKG_CONFIG}")

set(_crypto_lib "${_fido_lib}/libcrypto.a")
set(_fido2_lib "${_fido_lib}/libfido2.a")
set(_cbor_lib "${_fido_lib}/libcbor.a")

# OpenSSL: libcrypto only, no shared libraries, modules, tests, documentation or command-line tool.
set(_ossl_flags)
if(APPLE)
  set(_ossl_arch "${CMAKE_OSX_ARCHITECTURES}")
  if(NOT _ossl_arch)
    set(_ossl_arch "${CMAKE_HOST_SYSTEM_PROCESSOR}")
  endif()
  list(LENGTH _ossl_arch _ossl_count)
  if(_ossl_count GREATER 1)
    message(FATAL_ERROR "OpenSSL builds one architecture at a time; set CMAKE_OSX_ARCHITECTURES to one")
  endif()
  set(_ossl_target "darwin64-${_ossl_arch}-cc")
  if(CMAKE_OSX_DEPLOYMENT_TARGET)
    list(APPEND _ossl_flags "-mmacosx-version-min=${CMAKE_OSX_DEPLOYMENT_TARGET}")
  endif()
  if(CMAKE_OSX_SYSROOT)
    list(APPEND _ossl_flags -isysroot "${CMAKE_OSX_SYSROOT}")
  endif()
elseif(MINGW)
  set(_ossl_target mingw64)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
  set(_ossl_target linux-aarch64)
else()
  set(_ossl_target linux-x86_64)
endif()
set(_ossl_env "${CMAKE_COMMAND}" -E env "CC=${CMAKE_C_COMPILER}" "AR=${CMAKE_AR}" "RANLIB=${CMAKE_RANLIB}")
if(CMAKE_RC_COMPILER)
  list(APPEND _ossl_env "RC=${CMAKE_RC_COMPILER}")
endif()
include(ProcessorCount)
ProcessorCount(_jobs)
if(_jobs EQUAL 0)
  set(_jobs 4)
endif()
# OpenSSL compiles its directories into libcrypto. A neutral prefix keeps the build path out of the binary; the files are
# installed under a staging directory and copied into the shared prefix. Configuration files are never loaded on their own.
set(_ossl_prefix /opt/room237-openssl)
set(_ossl_stage "${_fido_prefix}-build/openssl-stage")
ExternalProject_Add(room237_openssl
  SOURCE_DIR "${_third}/openssl"
  PREFIX "${_fido_prefix}-build/openssl-steps"
  BINARY_DIR "${_fido_prefix}-build/openssl"
  CONFIGURE_COMMAND ${_ossl_env} "${ROOM237_PERL}" <SOURCE_DIR>/Configure ${_ossl_target} ${_ossl_flags}
    "--prefix=${_ossl_prefix}" --libdir=lib --openssldir=/etc/ssl
    no-shared no-module no-tests no-docs no-apps no-legacy no-engine no-ui-console no-quic no-autoload-config -fPIC
  BUILD_COMMAND "${ROOM237_MAKE}" -j${_jobs} build_libs
  INSTALL_COMMAND "${ROOM237_MAKE}" install_dev "DESTDIR=${_ossl_stage}"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${_ossl_stage}${_ossl_prefix}/include" "${_fido_prefix}/include"
    COMMAND "${CMAKE_COMMAND}" -E copy "${_ossl_stage}${_ossl_prefix}/lib/libcrypto.a" "${_fido_lib}/libcrypto.a"
  BUILD_BYPRODUCTS "${_crypto_lib}"
  UPDATE_COMMAND "" PATCH_COMMAND "")

ExternalProject_Add(room237_libcbor
  SOURCE_DIR "${_third}/libcbor"
  PREFIX "${_fido_prefix}-build/libcbor-steps"
  BINARY_DIR "${_fido_prefix}-build/libcbor"
  LIST_SEPARATOR |
  CMAKE_ARGS ${_fido_common} -DWITH_TESTS=OFF -DWITH_EXAMPLES=OFF -DSANITIZE=OFF -DCBOR_PRETTY_PRINTER=OFF
  BUILD_BYPRODUCTS "${_cbor_lib}"
  UPDATE_COMMAND "" PATCH_COMMAND "")

# The core links its own zlib; libfido2 needs only the headers to configure. libcrypto is described here as well, since
# OpenSSL's own description names the neutral prefix.
file(READ "${_lock}" _lock_text)
string(JSON _zlib_version GET "${_lock_text}" versions zlib)
string(JSON _ossl_version GET "${_lock_text}" versions openssl)
file(MAKE_DIRECTORY "${_fido_lib}/pkgconfig" "${_fido_prefix}/include")
file(WRITE "${_fido_lib}/pkgconfig/zlib.pc" "Name: zlib\nDescription: zlib\nVersion: ${_zlib_version}\nCflags: -I${_third}/zlib\nLibs:\n")
file(WRITE "${_fido_lib}/pkgconfig/libcrypto.pc" "Name: OpenSSL-libcrypto\nDescription: OpenSSL cryptography library\nVersion: ${_ossl_version}\nCflags: -I${_fido_prefix}/include\nLibs: -L${_fido_lib} -lcrypto\n")

# libfido2 always compiles with -Werror; newer compilers warn about its Windows paths (an unused parameter, a 32-bit
# u_long compared with SIZE_MAX, a prototype without parameters in its copy of webauthn.h). Those stay warnings; every
# other diagnostic keeps failing the build.
set(_fido2_cflags "-Wno-error=unused-parameter -Wno-error=deprecated-declarations -Wno-error=strict-prototypes")
set(_fido2_extra)
if(MINGW)
  # mingw-w64 defines clock_gettime inline in pthread_time.h, which libfido2's check does not see.
  list(APPEND _fido2_extra -DHAVE_CLOCK_GETTIME=1)
endif()
if(CMAKE_C_COMPILER_ID MATCHES "Clang")
  string(APPEND _fido2_cflags " -Wno-error=tautological-constant-out-of-range-compare -Wno-error=unknown-warning-option")
endif()

ExternalProject_Add(room237_libfido2
  SOURCE_DIR "${_third}/libfido2"
  PREFIX "${_fido_prefix}-build/libfido2-steps"
  BINARY_DIR "${_fido_prefix}-build/libfido2"
  DEPENDS room237_openssl room237_libcbor
  LIST_SEPARATOR |
  CONFIGURE_COMMAND ${_pkg_env} "${CMAKE_COMMAND}" -S <SOURCE_DIR> -B <BINARY_DIR> ${_fido_common}
    -DBUILD_TESTS=OFF -DBUILD_EXAMPLES=OFF -DBUILD_MANPAGES=OFF -DBUILD_TOOLS=OFF -DBUILD_STATIC_LIBS=ON
    "-DCMAKE_C_FLAGS=${_fido2_cflags}" ${_fido2_extra} -DNFC_LINUX=OFF -DUSE_PCSC=OFF -DUSE_HIDAPI=OFF -DUSE_WINHELLO=ON "-DPKG_CONFIG_EXECUTABLE=${ROOM237_PKG_CONFIG}"
  BUILD_COMMAND "${CMAKE_COMMAND}" --build <BINARY_DIR>
  INSTALL_COMMAND "${CMAKE_COMMAND}" --install <BINARY_DIR>
  BUILD_BYPRODUCTS "${_fido2_lib}"
  UPDATE_COMMAND "" PATCH_COMMAND "")

add_library(room237_fido INTERFACE)
add_dependencies(room237_fido room237_libfido2)
target_include_directories(room237_fido SYSTEM INTERFACE "${_fido_prefix}/include")
target_link_libraries(room237_fido INTERFACE "${_fido2_lib}" "${_cbor_lib}" "${_crypto_lib}")
target_compile_definitions(room237_fido INTERFACE ROOM237_FIDO)
if(APPLE)
  target_link_libraries(room237_fido INTERFACE "-framework IOKit" "-framework CoreFoundation")
elseif(WIN32)
  # clock_gettime comes from winpthreads, linked statically so that the package needs no further DLL.
  target_link_libraries(room237_fido INTERFACE setupapi hid bcrypt ws2_32 crypt32 user32 advapi32 "-l:libwinpthread.a")
else()
  target_link_libraries(room237_fido INTERFACE udev pthread ${CMAKE_DL_LIBS})
endif()
