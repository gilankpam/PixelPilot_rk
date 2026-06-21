cmake_minimum_required(VERSION 3.5)

project(pixelpilot VERSION 1.3.0)
find_package(PkgConfig REQUIRED)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Option to control wmenu simulator build
option(USE_SIMULATOR "Build the simulator version of the application" OFF)
# If build should include tests
option(BUILD_TESTS "Build unit tests" OFF)

# LVGL setup
if(NOT USE_SIMULATOR)
  set(LV_USE_LINUX_DRM ON)
endif()
# LVGL 9.x builds its demos/examples as separate libs and PUBLIC-links them
# into the `lvgl` target (env_support/cmake/os_desktop.cmake), so anything
# linking lvgl inherits a runtime dependency on liblvgl_demos/liblvgl_examples.
# The app uses neither — turn them off. (The gate is CONFIG_LV_BUILD_*, default
# ON; the old LV_CONF_BUILD_DISABLE_* names are not read by 9.5.0.)
set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)

add_subdirectory(lvgl)

# Set include directories for LVGL BEFORE any other targets
target_include_directories(lvgl PUBLIC 
    ${PROJECT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/lvgl
)
set(CMAKE_C_STANDARD 99) # LVGL officially supports C99 and above
set(CMAKE_CXX_STANDARD 17) #C17
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT USE_SIMULATOR)
  pkg_check_modules(LIBDRM REQUIRED libdrm)
  pkg_check_modules(CAIRO IMPORTED_TARGET REQUIRED cairo)
endif()
pkg_check_modules(SPDLOG REQUIRED spdlog)

find_package(PNG REQUIRED)
find_package(fmt REQUIRED)
find_package(spdlog REQUIRED)

include_directories(${PNG_INCLUDE_DIR})

# Simulator
set(SIMULATOR_SOURCES
        src/simulator.c
        src/gsmenu/ui.h
        src/gsmenu/helper.h
        src/gsmenu/helper.c
        src/gsmenu/styles.h
        src/gsmenu/styles.c
        src/gsmenu/widget_demo.c
        src/gsmenu/widgets/pp_section_header.h
        src/gsmenu/widgets/pp_section_header.c
        src/gsmenu/widgets/pp_row.h
        src/gsmenu/widgets/pp_row.c
        src/gsmenu/widgets/pp_toggle.h
        src/gsmenu/widgets/pp_toggle.c
        src/gsmenu/widgets/pp_slider.h
        src/gsmenu/widgets/pp_slider.c
        src/gsmenu/widgets/pp_slider_bounds.h
        src/gsmenu/widgets/pp_slider_bounds.c
        src/gsmenu/widgets/pp_slider_accel.h
        src/gsmenu/widgets/pp_slider_accel.c
        src/gsmenu/widgets/pp_slider_scale.h
        src/gsmenu/widgets/pp_slider_scale.c
        src/gsmenu/widgets/pp_dropdown.h
        src/gsmenu/widgets/pp_dropdown.c
        src/gsmenu/widgets/pp_page.h
        src/gsmenu/widgets/pp_page.c
        src/gsmenu/widgets/pp_tabbar.h
        src/gsmenu/widgets/pp_tabbar.c
        src/gsmenu/widgets/pp_drilldown.h
        src/gsmenu/widgets/pp_drilldown.c
        src/gsmenu/widgets/pp_toast.h
        src/gsmenu/widgets/pp_toast.c
        src/gsmenu/widgets/pp_footer.h
        src/gsmenu/widgets/pp_footer.c
        src/gsmenu/pages/pixelpilot.h
        src/gsmenu/pages/pixelpilot.c
        src/gsmenu/pages/camera.h
        src/gsmenu/pages/camera.c
        src/gsmenu/pages/link.h
        src/gsmenu/pages/link.c
        src/gsmenu/pages/dynamiclink.h
        src/gsmenu/pages/dynamiclink.c
        src/gsmenu/pages/system.h
        src/gsmenu/pages/system.c
        src/gsmenu/settings.h
        src/gsmenu/settings.c
        src/gsmenu/settings_stub.c
        src/gsmenu/settings_dummy.c
        src/gsmenu/settings_gs_enum.c
        third_party/cjson/cJSON.c
        src/lvosd.h
        src/lvosd.c
        src/menu.h
        src/menu.c
        src/input.h
        src/input.cpp
)

add_compile_options("-Wno-address-of-packed-member")

set(LIB_SOURCE_FILES 
        src/drm.h
        src/drm.c
        src/gsmenu/ui.h
        src/gsmenu/helper.h
        src/gsmenu/helper.c
        src/gsmenu/styles.h
        src/gsmenu/styles.c
        src/gsmenu/widgets/pp_section_header.h
        src/gsmenu/widgets/pp_section_header.c
        src/gsmenu/widgets/pp_row.h
        src/gsmenu/widgets/pp_row.c
        src/gsmenu/widgets/pp_toggle.h
        src/gsmenu/widgets/pp_toggle.c
        src/gsmenu/widgets/pp_slider.h
        src/gsmenu/widgets/pp_slider.c
        src/gsmenu/widgets/pp_slider_bounds.h
        src/gsmenu/widgets/pp_slider_bounds.c
        src/gsmenu/widgets/pp_slider_accel.h
        src/gsmenu/widgets/pp_slider_accel.c
        src/gsmenu/widgets/pp_slider_scale.h
        src/gsmenu/widgets/pp_slider_scale.c
        src/gsmenu/widgets/pp_dropdown.h
        src/gsmenu/widgets/pp_dropdown.c
        src/gsmenu/widgets/pp_page.h
        src/gsmenu/widgets/pp_page.c
        src/gsmenu/widgets/pp_tabbar.h
        src/gsmenu/widgets/pp_tabbar.c
        src/gsmenu/widgets/pp_drilldown.h
        src/gsmenu/widgets/pp_drilldown.c
        src/gsmenu/widgets/pp_toast.h
        src/gsmenu/widgets/pp_toast.c
        src/gsmenu/widgets/pp_footer.h
        src/gsmenu/widgets/pp_footer.c
        src/gsmenu/pages/pixelpilot.h
        src/gsmenu/pages/pixelpilot.c
        src/gsmenu/pages/camera.h
        src/gsmenu/pages/camera.c
        src/gsmenu/pages/link.h
        src/gsmenu/pages/link.c
        src/gsmenu/pages/dynamiclink.h
        src/gsmenu/pages/dynamiclink.c
        src/gsmenu/pages/system.h
        src/gsmenu/pages/system.c
        src/gsmenu/settings.h
        src/gsmenu/settings.c
        src/gsmenu/settings_stub.c
        src/lvosd.h
        src/lvosd.c
        src/menu.h
        src/menu.c
        src/input.h
        src/input.cpp
        src/osd.h
        src/osd.hpp
        src/osd.cpp
        src/osd_aio_logic.hpp
        src/osd_aio_logic.cpp
        src/os_mon.hpp
        src/os_mon.cpp
        src/osd_gl.cpp
        src/dvr.h
        src/dvr.cpp
        src/mpp_encoder.h
        src/mpp_encoder.cpp
        src/frame_processor.h
        src/frame_processor.cpp
        src/frame_colorcorrect.h
        src/frame_colorcorrect.cpp
        src/mavlink.h
        src/mavlink.c
        src/wfbcli.hpp
        src/wfbcli.cpp
        src/WiFiRSSIMonitor.hpp
        src/WiFiRSSIMonitor.cpp
        src/scheduling_helper.hpp
        src/latency_probe.hpp
        src/latency_probe.cpp
        src/gst_file_player.cpp
        src/gst_file_player.h
        src/rtp_video_receiver.cpp
        src/rtp_video_receiver.h
        src/hevc_depayloader.cpp
        src/hevc_depayloader.h)
set(SOURCE_FILES
  ${LIB_SOURCE_FILES}
  src/main.cpp
  src/main.h)
file(GLOB ICONS src/icons/*.png)
file(GLOB OSD_CONFIGS *_osd.json)

configure_file("${PROJECT_NAME}_config.h.in" "${PROJECT_NAME}_config.h")

if(USE_SIMULATOR)
  add_definitions(-DUSE_SIMULATOR)
  target_compile_definitions(lvgl PRIVATE USE_SIMULATOR)
  pkg_check_modules(LIBSDL REQUIRED sdl2)
  add_executable(${PROJECT_NAME} ${SIMULATOR_SOURCES})
  target_link_libraries(${PROJECT_NAME} lvgl ${LIBSDL_LIBRARIES} ${PNG_LIBRARIES})
  target_include_directories(${PROJECT_NAME} PRIVATE
      ${LIBSDL_INCLUDE_DIRS}
      ${PROJECT_SOURCE_DIR}/third_party/cjson)

  # Standalone tests for the settings dispatcher — links only what it needs,
  # avoiding the heavy rockchip/drm/gst deps that pixelpilot_tests pulls in.
  find_package(Catch2 QUIET)
  if(Catch2_FOUND)
    get_target_property(PP_CATCH2_INC Catch2::Catch2 INTERFACE_INCLUDE_DIRECTORIES)
    # Nix clang-wrapper: Catch2 include ends up in CMAKE_CXX_IMPLICIT_INCLUDE_DIRECTORIES,
    # so CMake deduplicates it out of per-target flags. Force it via CXX_FLAGS.
    string(APPEND CMAKE_CXX_FLAGS " -I${PP_CATCH2_INC}")
    set(PP_CJSON_SOURCES ${PROJECT_SOURCE_DIR}/third_party/cjson/cJSON.c)
    set(PP_CJSON_INC     ${PROJECT_SOURCE_DIR}/third_party/cjson)
    find_package(CURL REQUIRED)

    add_executable(settings_tests
      src/gsmenu/settings.c
      src/gsmenu/settings_dummy.c
      src/gsmenu/settings_stub.c
      src/gsmenu/settings_gs_enum.c
      ${PP_CJSON_SOURCES}
      src/gsmenu/widgets/pp_slider_bounds.c
      src/gsmenu/widgets/pp_slider_accel.c
      src/gsmenu/widgets/pp_slider_scale.c
      tests/test_settings.cpp
      tests/test_settings_failure.cpp
      tests/test_slider_bounds.cpp
      tests/test_slider_accel.cpp
      tests/test_slider_scale.cpp)
    target_include_directories(settings_tests PRIVATE
      ${PROJECT_SOURCE_DIR}/src
      ${PP_CJSON_INC})
    target_link_libraries(settings_tests Catch2::Catch2WithMain lvgl ${PNG_LIBRARIES})

    add_executable(lvgl_state_tests
      tests/test_lvgl_alt_state.cpp)
    target_include_directories(lvgl_state_tests PRIVATE
      ${PROJECT_SOURCE_DIR})
    target_link_libraries(lvgl_state_tests Catch2::Catch2WithMain lvgl ${PNG_LIBRARIES})

    # fpvd provider unit + integration tests
    add_executable(fpvd_tests
      src/gsmenu/settings.c
      src/gsmenu/settings_fpvd.c
      src/gsmenu/settings_gs_enum.c
      ${PP_CJSON_SOURCES}
      tests/test_settings_fpvd.cpp
      tests/test_settings_fpvd_integration.cpp)
    target_include_directories(fpvd_tests PRIVATE
      ${PROJECT_SOURCE_DIR}/src
      ${PP_CJSON_INC}
      ${PROJECT_SOURCE_DIR}/third_party/cpp-httplib)
    target_link_libraries(fpvd_tests
      Catch2::Catch2WithMain lvgl ${PNG_LIBRARIES} CURL::libcurl pthread)
    target_compile_definitions(fpvd_tests PRIVATE PP_FPVD_TEST=1)

    add_executable(gs_enum_tests
      src/gsmenu/settings_gs_enum.c
      ${PP_CJSON_SOURCES}
      tests/test_settings_gs_enum.cpp)
    target_include_directories(gs_enum_tests PRIVATE
      ${PROJECT_SOURCE_DIR}/src
      ${PP_CJSON_INC})
    target_link_libraries(gs_enum_tests
      Catch2::Catch2WithMain lvgl ${PNG_LIBRARIES})

    add_executable(runtime_cfg_tests
      src/gsmenu/settings_runtime_cfg.c
      ${PP_CJSON_SOURCES}
      tests/test_settings_runtime_cfg.cpp)
    target_include_directories(runtime_cfg_tests PRIVATE
      ${PROJECT_SOURCE_DIR}/src
      ${PROJECT_SOURCE_DIR}/src/gsmenu
      ${PP_CJSON_INC})
    target_link_libraries(runtime_cfg_tests
      Catch2::Catch2WithMain)

    add_executable(aio_logic_tests
      src/osd_aio_logic.cpp
      tests/test_aio_logic.cpp)
    target_include_directories(aio_logic_tests PRIVATE
      ${PROJECT_SOURCE_DIR}/src)
    target_link_libraries(aio_logic_tests Catch2::Catch2WithMain)

    add_executable(osd_buf_tests
      tests/test_osd_buf.cpp)
    target_include_directories(osd_buf_tests PRIVATE
      ${PROJECT_SOURCE_DIR}/src)
    target_link_libraries(osd_buf_tests Catch2::Catch2WithMain)

    add_executable(dvr_timing_tests
      tests/test_dvr_timing.cpp)
    target_include_directories(dvr_timing_tests PRIVATE
      ${PROJECT_SOURCE_DIR}/src)
    target_link_libraries(dvr_timing_tests Catch2::Catch2WithMain)

    add_executable(dvr_fps_cap_tests
      tests/test_dvr_fps_cap.cpp)
    target_include_directories(dvr_fps_cap_tests PRIVATE
      ${PROJECT_SOURCE_DIR}/src)
    target_link_libraries(dvr_fps_cap_tests Catch2::Catch2WithMain)

    add_executable(dvr_moov_duration_tests
      tests/test_dvr_moov_duration.cpp)
    target_include_directories(dvr_moov_duration_tests PRIVATE
      ${PROJECT_SOURCE_DIR}/src)
    target_link_libraries(dvr_moov_duration_tests Catch2::Catch2WithMain)

    add_executable(rtp_depay_tests
      src/hevc_depayloader.cpp
      tests/test_hevc_depayloader.cpp)
    target_include_directories(rtp_depay_tests PRIVATE
      ${PROJECT_SOURCE_DIR}/src
      ${PROJECT_SOURCE_DIR})
    target_link_libraries(rtp_depay_tests Catch2::Catch2WithMain)

    option(BUILD_FUZZERS "Build libFuzzer targets" OFF)
    if(BUILD_FUZZERS)
      add_executable(rtp_depay_fuzz
        src/hevc_depayloader.cpp
        tests/fuzz_hevc_depayloader.cpp)
      target_include_directories(rtp_depay_fuzz PRIVATE
        ${PROJECT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR})
      target_compile_options(rtp_depay_fuzz PRIVATE -fsanitize=fuzzer,address,undefined)
      target_link_options(rtp_depay_fuzz PRIVATE -fsanitize=fuzzer,address,undefined)
    endif()

    # Hidden native widgets (lv_dropdown/lv_switch) must never be
    # group-focusable — see tests/test_widget_focus.cpp.
    add_executable(widget_focus_tests
      src/gsmenu/widgets/pp_dropdown.c
      src/gsmenu/widgets/pp_toggle.c
      src/gsmenu/widgets/pp_row.c
      src/gsmenu/widgets/pp_toast.c
      src/gsmenu/styles.c
      src/gsmenu/settings.c
      src/gsmenu/settings_stub.c
      src/gsmenu/settings_gs_enum.c
      ${PP_CJSON_SOURCES}
      tests/test_widget_focus.cpp)
    target_include_directories(widget_focus_tests PRIVATE
      ${PROJECT_SOURCE_DIR}/src
      ${PROJECT_SOURCE_DIR}
      ${PP_CJSON_INC})
    target_link_libraries(widget_focus_tests Catch2::Catch2WithMain lvgl ${PNG_LIBRARIES})

    # A page whose rows are all lock-disabled (drone off) must never trap
    # keyboard focus — see tests/test_page_nav_lock.cpp.
    add_executable(page_nav_tests
      src/gsmenu/widgets/pp_page.c
      src/gsmenu/widgets/pp_tabbar.c
      src/gsmenu/widgets/pp_toggle.c
      src/gsmenu/widgets/pp_dropdown.c
      src/gsmenu/widgets/pp_row.c
      src/gsmenu/widgets/pp_toast.c
      src/gsmenu/styles.c
      src/gsmenu/helper.c
      src/gsmenu/settings.c
      src/gsmenu/settings_gs_enum.c
      ${PP_CJSON_SOURCES}
      tests/test_page_nav_lock.cpp)
    target_include_directories(page_nav_tests PRIVATE
      ${PROJECT_SOURCE_DIR}/src
      ${PROJECT_SOURCE_DIR}
      ${PP_CJSON_INC})
    target_link_libraries(page_nav_tests Catch2::Catch2WithMain lvgl ${PNG_LIBRARIES})

    # The Dynamic Link page must not build deprecated Failsafe rows
    # (Block Depth / Bandwidth / TX Power) — see tests/test_dynamiclink_page.cpp.
    add_executable(dynamiclink_page_tests
      src/gsmenu/pages/dynamiclink.c
      src/gsmenu/widgets/pp_page.c
      src/gsmenu/widgets/pp_section_header.c
      src/gsmenu/widgets/pp_toggle.c
      src/gsmenu/widgets/pp_slider.c
      src/gsmenu/widgets/pp_slider_bounds.c
      src/gsmenu/widgets/pp_slider_accel.c
      src/gsmenu/widgets/pp_slider_scale.c
      src/gsmenu/widgets/pp_dropdown.c
      src/gsmenu/widgets/pp_row.c
      src/gsmenu/widgets/pp_toast.c
      src/gsmenu/styles.c
      src/gsmenu/helper.c
      src/gsmenu/settings.c
      src/gsmenu/settings_stub.c
      src/gsmenu/settings_gs_enum.c
      ${PP_CJSON_SOURCES}
      tests/test_dynamiclink_page.cpp)
    target_include_directories(dynamiclink_page_tests PRIVATE
      ${PROJECT_SOURCE_DIR}/src
      ${PROJECT_SOURCE_DIR}
      ${PP_CJSON_INC})
    target_link_libraries(dynamiclink_page_tests Catch2::Catch2WithMain lvgl ${PNG_LIBRARIES})

  endif()
else()
  find_package(nlohmann_json 3 REQUIRED)
  find_package(yaml-cpp REQUIRED)
  find_package(CURL REQUIRED)
  pkg_check_modules(LIBGPIOD REQUIRED libgpiod)
  include_directories(${LIBGPIOD_INCLUDE_DIR})

  find_path(RGA_INCLUDE_DIRS NAMES rga.h rga/rga.h)
  find_library(RGA_LIBRARIES NAMES rga librga)

  # GStreamer setup
  find_package(PkgConfig REQUIRED)
  pkg_search_module(GST REQUIRED
          gstreamer-1.0>=1.4
          gstreamer-app-1.0>=1.4
  )
  pkg_search_module(gstreamer REQUIRED IMPORTED_TARGET gstreamer-1.0>=1.4)
  pkg_search_module(gstreamer-app REQUIRED IMPORTED_TARGET gstreamer-app-1.0>=1.4)

  # fpvd provider sources (device build)
  list(APPEND SOURCE_FILES
      src/gsmenu/settings_fpvd.c
      src/gsmenu/osd_air_bridge.h
      src/gsmenu/osd_air_bridge.c
      third_party/cjson/cJSON.c
  )

  # GS-side settings backend (enumeration helpers)
  list(APPEND SOURCE_FILES
      src/gsmenu/settings_gs_enum.c)

  add_executable(${PROJECT_NAME} ${SOURCE_FILES})

  # Add include directories for the main target
  target_include_directories(${PROJECT_NAME} PRIVATE
      ${PROJECT_SOURCE_DIR}
      ${PROJECT_SOURCE_DIR}/lvgl
      ${PROJECT_SOURCE_DIR}/third_party/cjson
      ${LIBDRM_INCLUDE_DIRS}
      ${RGA_INCLUDE_DIRS}
  )

  target_link_libraries(${PROJECT_NAME}
      rockchip_mpp
      pthread
      drm
      m
      PkgConfig::CAIRO
      spdlog::spdlog
      fmt::fmt
      nlohmann_json::nlohmann_json
      yaml-cpp
      rt
      lvgl
      ${LIBGPIOD_LIBRARIES}
      ${PNG_LIBRARIES}
      PkgConfig::gstreamer
      PkgConfig::gstreamer-app
      ${RGA_LIBRARIES}
      EGL GLESv2 gbm
      CURL::libcurl
  )
endif()

if(CMAKE_BUILD_TYPE MATCHES "Debug")
  add_compile_definitions(SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_TRACE)
  set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
  set(
    CMAKE_C_FLAGS
    "${CMAKE_C_FLAGS} -Werror -fsanitize=undefined -fsanitize=address"
  )
  target_link_options(${PROJECT_NAME}
    BEFORE PUBLIC -fsanitize=undefined PUBLIC -fsanitize=address
  )
endif()

target_include_directories(${PROJECT_NAME} PUBLIC "${PROJECT_BINARY_DIR}")

# Testing
if(BUILD_TESTS)
    find_package(Catch2 REQUIRED)

    # Test source files
    set(TEST_SOURCES
      tests/test_osd.cpp
      tests/test_video_stutter.cpp
      tests/test_latency_probe.cpp
      tests/test_latency_probe_integration.cpp
      src/main.h
      src/main.cpp
    )
    find_package(spdlog REQUIRED)
    find_package(nlohmann_json 3 REQUIRED)
    find_package(yaml-cpp REQUIRED)

    # Test executable
    add_executable(pixelpilot_tests ${LIB_SOURCE_FILES} ${TEST_SOURCES})
    target_include_directories(pixelpilot_tests PUBLIC "${PROJECT_BINARY_DIR}" ${LIBDRM_INCLUDE_DIRS} ${CAIRO_INCLUDE_DIRS})
    # Define the TEST preprocessor directive
    target_compile_definitions(pixelpilot_tests PRIVATE TEST)
    target_link_libraries(pixelpilot_tests rockchip_mpp pthread drm m cairo spdlog::spdlog nlohmann_json::nlohmann_json yaml-cpp rt lvgl ${LIBGPIOD_LIBRARIES} ${PNG_LIBRARIES} Catch2::Catch2WithMain)

    find_package(PkgConfig REQUIRED)
    pkg_search_module(GST REQUIRED
          gstreamer-1.0>=1.4
          gstreamer-app-1.0>=1.4
    )
    pkg_search_module(gstreamer REQUIRED IMPORTED_TARGET gstreamer-1.0>=1.4)
    pkg_search_module(gstreamer-app REQUIRED IMPORTED_TARGET gstreamer-app-1.0>=1.4)
    target_link_libraries(pixelpilot_tests PkgConfig::gstreamer PkgConfig::gstreamer-app ${RGA_LIBRARIES} EGL GLESv2 gbm)

    target_link_options(pixelpilot_tests
      BEFORE PUBLIC -fsanitize=undefined PUBLIC -fsanitize=address
    )

    # Optionally, establish testing options
    #enable_testing()
    #add_test(NAME PixelPilotTests COMMAND pixelpilot_tests)
endif()


# Installation

include(GNUInstallDirs)
install(TARGETS ${PROJECT_NAME}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(FILES ${ICONS}
    DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/${PROJECT_NAME}
)
# Shared fonts dir: fontconfig-scanned (the Cairo OSD resolves "Barlow
# Condensed" by family name) and on gsmenu's lv_tiny_ttf search path.
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/src/gsmenu/fonts/BarlowCondensed-Medium.ttf
              ${CMAKE_CURRENT_SOURCE_DIR}/src/gsmenu/fonts/BarlowCondensed-ExtraBold.ttf
    DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/fonts
)

# Install configuration files to /etc
install(FILES ${OSD_CONFIGS}
    DESTINATION ${CMAKE_INSTALL_SYSCONFDIR}/${PROJECT_NAME}
)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/pixelpilot.yaml
    DESTINATION ${CMAKE_INSTALL_SYSCONFDIR}/${PROJECT_NAME}
)
