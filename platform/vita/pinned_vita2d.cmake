# Public MIT libvita2d, pinned with its existing compiled homebrew shaders.
if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()
include(FetchContent)
FetchContent_Declare(buffered_vita2d_source
    URL https://codeload.github.com/xerpi/libvita2d/tar.gz/a8f15ab09d5233f0a4e4ad0e8f6ade0da888cbed
    URL_HASH SHA256=97b48d7955882b283d67450936e1ca04aad1549f733141d5b0fa7953cf805bbc)
FetchContent_MakeAvailable(buffered_vita2d_source)
set(V2D_ROOT "${buffered_vita2d_source_SOURCE_DIR}/libvita2d")
file(GLOB V2D_C "${V2D_ROOT}/source/*.c")
file(GLOB V2D_SHADERS "${V2D_ROOT}/shader/compiled/*.o")
set_source_files_properties(${V2D_SHADERS} PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)
add_library(daytona_vita2d STATIC ${V2D_C} ${V2D_SHADERS})
target_compile_options(daytona_vita2d PRIVATE -include math.h)
target_include_directories(daytona_vita2d PRIVATE "${V2D_ROOT}/include" "${CMAKE_CURRENT_SOURCE_DIR}" "${VITASDK}/arm-vita-eabi/include/freetype2")
set(VITA2D_INCLUDE_DIR "${V2D_ROOT}/include")
set(VITA2D_LIBRARY daytona_vita2d)
