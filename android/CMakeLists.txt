cmake_minimum_required(VERSION 3.12)

project(ImGuiExample)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# ##########################################################################################################################################
include_directories(deps/output/${ANDROID_ABI}/include)

set(ANDROID_DEPS_DIR ${PROJECT_SOURCE_DIR}/deps)
set(ANDROID_DEPS_LIB_DIR ${ANDROID_DEPS_DIR}/output/${ANDROID_ABI}/lib)

if(NOT EXISTS ${ANDROID_DEPS_DIR}/build.sh)
  message(FATAL_ERROR
    "Android dependencies are missing (${ANDROID_DEPS_DIR}).\n"
    "Run: git submodule update --init android/deps\n"
    "Then build them: cd android/deps && sh build.sh"
  )
endif()

if(NOT EXISTS ${ANDROID_DEPS_LIB_DIR})
  message(FATAL_ERROR
    "Android dependency libraries were not built for ABI ${ANDROID_ABI}.\n"
    "Missing directory: ${ANDROID_DEPS_LIB_DIR}\n"
    "Run: cd android/deps && sh build.sh"
  )
endif()

# LIBVOLK.  CMake may install a versioned shared object (for example
# libvolk.so.3.1.2) without an unversioned symlink on Android, so accept both
# static and versioned shared library names emitted by android/deps.
set(VOLK_SEARCH_PATH ${ANDROID_DEPS_LIB_DIR})
file(GLOB VOLK_CANDIDATES LIST_DIRECTORIES false
  "${VOLK_SEARCH_PATH}/libvolk.a"
  "${VOLK_SEARCH_PATH}/libvolk_static.a"
  "${VOLK_SEARCH_PATH}/libvolk*.a"
  "${VOLK_SEARCH_PATH}/libvolk.so"
  "${VOLK_SEARCH_PATH}/libvolk.so.*"
)
list(SORT VOLK_CANDIDATES)
set(VOLK_LIBRARY "")
if(VOLK_CANDIDATES)
  list(GET VOLK_CANDIDATES 0 VOLK_LIBRARY)
endif()

if(NOT VOLK_LIBRARY OR NOT EXISTS "${VOLK_LIBRARY}")
  file(GLOB AVAILABLE_ANDROID_DEPS LIST_DIRECTORIES false "${VOLK_SEARCH_PATH}/*")
  list(JOIN AVAILABLE_ANDROID_DEPS "\n    " AVAILABLE_ANDROID_DEPS_TEXT)
  message(FATAL_ERROR
    "VOLK library not found in ${VOLK_SEARCH_PATH}. Expected libvolk*.a or libvolk.so*.\n"
    "Files present:\n    ${AVAILABLE_ANDROID_DEPS_TEXT}\n"
    "If android/deps was not built yet, run: cd android/deps && sh build.sh"
  )
endif()

add_library(volk UNKNOWN IMPORTED)
set_target_properties(volk PROPERTIES IMPORTED_LOCATION ${VOLK_LIBRARY})
# include_directories(deps/volk/jni/)

# LIBFFTW3
add_library(fftw STATIC IMPORTED)
set_target_properties(fftw PROPERTIES IMPORTED_LOCATION ${PROJECT_SOURCE_DIR}/deps/output/${ANDROID_ABI}/lib/libfftw3f.a)
# include_directories(deps/fftw3/jni)

# LIBPNG
add_library(png STATIC IMPORTED)
set_target_properties(png PROPERTIES IMPORTED_LOCATION ${PROJECT_SOURCE_DIR}/deps/output/${ANDROID_ABI}/lib/libpng.a)

# target_link_libraries(png zlib)
# include_directories(deps/libpng/jni/)

# LIBCPPFEATURES
add_library(cpufeatures STATIC IMPORTED)
set_target_properties(cpufeatures PROPERTIES IMPORTED_LOCATION ${PROJECT_SOURCE_DIR}/deps/output/${ANDROID_ABI}/lib/libcpu_features.a)
# include_directories(deps/volk/jni/)

# ZLIB
add_library(zlib STATIC IMPORTED)
set_target_properties(zlib PROPERTIES IMPORTED_LOCATION ${PROJECT_SOURCE_DIR}/deps/output/${ANDROID_ABI}/lib/libz.a)
# include_directories(deps/zlib/jni/)

# LIBJPEG
# add_library(jpeg STATIC IMPORTED)
# set_target_properties(jpeg PROPERTIES IMPORTED_LOCATION ${PROJECT_SOURCE_DIR}/deps/jpeg/${ANDROID_ABI}/libjpeg.a)
# include_directories(deps/jpeg/jni)

# MBed-TLS
add_library(wolfssl STATIC IMPORTED)
set_target_properties(wolfssl PROPERTIES IMPORTED_LOCATION ${PROJECT_SOURCE_DIR}/deps/output/${ANDROID_ABI}/lib/libwolfssl.a)

# NNG
add_library(nng STATIC IMPORTED)
set_target_properties(nng PROPERTIES IMPORTED_LOCATION ${PROJECT_SOURCE_DIR}/deps/output/${ANDROID_ABI}/lib/libnng.a)
# include_directories(deps/nng/jni/)

# cURL
add_library(curl STATIC IMPORTED)
set_target_properties(curl PROPERTIES IMPORTED_LOCATION ${PROJECT_SOURCE_DIR}/deps/output/${ANDROID_ABI}/lib/libcurl.a)
# include_directories(deps/curl/jni/)

# ZSTD
add_library(zstd STATIC IMPORTED)
set_target_properties(zstd PROPERTIES IMPORTED_LOCATION ${PROJECT_SOURCE_DIR}/deps/output/${ANDROID_ABI}/lib/libzstd.a)
# include_directories(deps/zstd/jni/)

# TIFF
add_library(tiff STATIC IMPORTED)
set_target_properties(tiff PROPERTIES IMPORTED_LOCATION ${PROJECT_SOURCE_DIR}/deps/output/${ANDROID_ABI}/lib/libtiff.a)
# include_directories(deps/tiff/jni/)

# ##########################################################################################################################################

# LibUSB
file(GLOB_RECURSE libusb_cpps deps/libusb/*.c)
add_library(usb STATIC ${libusb_cpps})
target_include_directories(usb PRIVATE deps/libusb/)

# LibAirspy
add_library(airspy STATIC deps/libairspy/airspy.c deps/libairspy/iqconverter_float.c deps/libairspy/iqconverter_int16.c)
target_include_directories(airspy PRIVATE deps/libairspy/ deps/libusb)
target_link_libraries(airspy PRIVATE usb)

# LibAirspyHF
add_library(airspyhf STATIC deps/libairspyhf/airspyhf.c deps/libairspyhf/iqbalancer.c)
target_include_directories(airspyhf PRIVATE deps/libairspyhf/ deps/libusb)
target_link_libraries(airspyhf PRIVATE usb)

# LibRTLSDR
file(GLOB_RECURSE librtlsdr_cpps deps/librtlsdr/*.c)
add_library(rtlsdr STATIC ${librtlsdr_cpps})
target_include_directories(rtlsdr PRIVATE deps/librtlsdr/ deps/libusb)
target_link_libraries(rtlsdr PRIVATE usb)

# LibHackRF
add_library(hackrf STATIC deps/libhackrf/hackrf.c)
target_include_directories(hackrf PRIVATE deps/libhackrf/ deps/libusb)
target_link_libraries(hackrf PRIVATE usb)

# LibLimeSuite
file(GLOB_RECURSE liblimesuite_cpps deps/liblimesuite/*.c deps/liblimesuite/*.cpp)
add_library(limesuite STATIC ${liblimesuite_cpps})
target_include_directories(limesuite PRIVATE deps/liblimesuite/ deps/liblimesuite/lms7002m deps/liblimesuite/API deps/liblimesuite/lime deps/liblimesuite/ConnectionRegistry deps/liblimesuite/lime deps/liblimesuite/threadHelper deps/liblimesuite/FPGA_common deps/liblimesuite/Si5351C deps/liblimesuite/lms7002m_mcu deps/liblimesuite/ADF4002 deps/liblimesuite/protocols deps/libusb)
target_link_libraries(limesuite PRIVATE usb)

# target_compile_definitions(limesuite PUBLIC LIBBLADERF_SEARCH_PREFIX="./")

# LibIIO
file(GLOB_RECURSE libiio_cpps deps/libiio/*.c)
add_library(iio STATIC ${libiio_cpps})
target_include_directories(iio PRIVATE deps/libiio/ deps/libiio/deps deps/libusb)
target_link_libraries(iio PRIVATE usb)

# LibAD9361-iio
file(GLOB_RECURSE libad9361iio_cpps deps/libad9361-iio/*.c)
add_library(ad9361iio STATIC ${libad9361iio_cpps})
target_include_directories(ad9361iio PRIVATE deps/libiio/ deps/libiio/deps deps/libad9361-iio)
target_link_libraries(ad9361iio PRIVATE iio)

# ##########################################################################################################################################
add_subdirectory(.. ${CMAKE_BINARY_DIR}/all_libs)

add_library(android_imgui SHARED
  ${CMAKE_CURRENT_SOURCE_DIR}/main.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/backend.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/imgui_image.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/imgui_backends/imgui_impl_android.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/imgui_backends/imgui_impl_opengl3.cpp
  ${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c
)

set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -u ANativeActivity_onCreate")
string(LENGTH "${CMAKE_SOURCE_DIR}/" SOURCE_PATH_SIZE)
add_compile_definitions(SOURCE_PATH_SIZE=${SOURCE_PATH_SIZE})

target_compile_definitions(android_imgui PRIVATE IMGUI_IMPL_OPENGL_ES3)

target_include_directories(android_imgui PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/imgui_backends
  ${ANDROID_NDK}/sources/android/native_app_glue
  ${CMAKE_CURRENT_SOURCE_DIR}/../src-core/
)

target_link_libraries(android_imgui PRIVATE
  android
  EGL
  GLESv3
  log
  satdump_core
  satdump_interface
)
