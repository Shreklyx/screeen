# Screen Share for macOS (native): "Screen Share.app" – AppKit, ScreenCaptureKit, VideoToolbox, Core Audio.
# One architecture per build (CMAKE_OSX_ARCHITECTURES); CI joins the arm64 and x86_64 builds with lipo.
set(SS_VERSION "2.0.0" CACHE STRING "version shown and compared for updates")

add_executable(ScreenShareMac MACOSX_BUNDLE
  main_mac.mm host_mac.mm player_mac.mm ws_mac.mm ../common/audio_ring.cpp)
set_target_properties(ScreenShareMac PROPERTIES
  OUTPUT_NAME "Screen Share"
  MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in
  MACOSX_BUNDLE_BUNDLE_VERSION ${SS_VERSION}
  MACOSX_BUNDLE_SHORT_VERSION_STRING ${SS_VERSION})
target_include_directories(ScreenShareMac PRIVATE . ../common)
target_compile_definitions(ScreenShareMac PRIVATE SS_VERSION="${SS_VERSION}")
set_source_files_properties(main_mac.mm host_mac.mm player_mac.mm ws_mac.mm PROPERTIES COMPILE_OPTIONS -fobjc-arc)
target_compile_options(ScreenShareMac PRIVATE -Wall -Wno-missing-field-initializers)
target_link_libraries(ScreenShareMac PRIVATE sscore opus
  "-framework AppKit" "-framework AVFoundation" "-framework CoreMedia" "-framework CoreVideo"
  "-framework VideoToolbox" "-framework ScreenCaptureKit" "-framework AudioToolbox" "-framework CoreAudio"
  "-framework QuartzCore" "-framework IOSurface" "-framework Security")

# the icon (app.icns) when one was made beside the sources (CI: sips + iconutil from desktop/build/icon.png)
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/app.icns)
  target_sources(ScreenShareMac PRIVATE app.icns)
  set_source_files_properties(app.icns PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
endif()
