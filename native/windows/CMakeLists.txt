# Screen Share for Windows (native): ScreenShare.exe
set(SS_VERSION "2.0.0" CACHE STRING "version shown and compared for updates")
string(REPLACE "." "," SS_VERSION_NUMBERS "${SS_VERSION},0")

add_executable(ScreenShare WIN32
  main.cpp host.cpp capture.cpp nvenc.cpp mfenc.cpp player.cpp audio.cpp update.cpp ws_winhttp.cpp app.rc)
target_include_directories(ScreenShare PRIVATE . ../third_party/nvenc)
target_compile_definitions(ScreenShare PRIVATE UNICODE _UNICODE _WIN32_WINNT=0x0A00 WINVER=0x0A00 SS_VERSION="${SS_VERSION}")
configure_file(version.h.in ${CMAKE_CURRENT_BINARY_DIR}/version.h)
target_include_directories(ScreenShare PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(ScreenShare PRIVATE sscore opus
  d3d11 dxgi mfplat mf mfuuid wmcodecdspuuid strmiids winhttp crypt32 comctl32 uxtheme winmm avrt ole32 oleaut32 shell32 user32 gdi32)
if(MSVC)
  target_compile_options(ScreenShare PRIVATE /W3 /utf-8)
  target_link_options(ScreenShare PRIVATE /MANIFEST:NO)
else()
  target_compile_options(ScreenShare PRIVATE -Wall -Wno-missing-field-initializers)
  target_link_options(ScreenShare PRIVATE -municode -static -static-libgcc -static-libstdc++)
endif()
