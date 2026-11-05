# The watch emulator (app: Watch emulator): the firmware's own screen code,
# built for the phone against the PC simulator's stubbed hardware
# (tools/watch_sim) - see tools/watch_sim/emu_main.cpp for the C interface
# the app calls through dart:ffi.
cmake_minimum_required(VERSION 3.18)
project(watchemu CXX)

set(REPO ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../..)
set(FW ${REPO}/firmware)
set(SIM ${REPO}/tools/watch_sim)

# The Arduino libraries the screen code uses (and JPEGDEC, for the images
# the app sends: icons, covers, the camera's live view), at the versions the
# firmware is built with (scripts/arduino-libraries.txt).
include(FetchContent)
FetchContent_Declare(arduino_gfx GIT_REPOSITORY https://github.com/moononournation/Arduino_GFX.git GIT_TAG v1.6.8 GIT_SHALLOW TRUE)
FetchContent_Declare(arduinojson GIT_REPOSITORY https://github.com/bblanchon/ArduinoJson.git GIT_TAG v7.4.3 GIT_SHALLOW TRUE)
FetchContent_Declare(jpegdec GIT_REPOSITORY https://github.com/bitbank2/JPEGDEC.git GIT_TAG 1.8.4 GIT_SHALLOW TRUE)
FetchContent_GetProperties(arduino_gfx)
if(NOT arduino_gfx_POPULATED)
  FetchContent_Populate(arduino_gfx)
endif()
FetchContent_GetProperties(arduinojson)
if(NOT arduinojson_POPULATED)
  FetchContent_Populate(arduinojson)
endif()

FetchContent_GetProperties(jpegdec)
if(NOT jpegdec_POPULATED)
  FetchContent_Populate(jpegdec)
endif()

# Which firmware files: all but the hardware drivers (emu_sources.sh,
# the same list as the PC simulator), so new firmware files come along.
file(READ ${SIM}/emu_sources.sh EMU_LIST)
string(REGEX MATCH "EMU_SKIP=\\(([^)]*)\\)" _ "${EMU_LIST}")
string(REGEX REPLACE "[ \t\r\n]+" ";" SKIP "${CMAKE_MATCH_1}")
file(GLOB FW_ALL ${FW}/*.cpp)
set(SRC ${SIM}/emu_main.cpp ${SIM}/sim_stubs.cpp ${jpegdec_SOURCE_DIR}/src/JPEGDEC.cpp)
foreach(f ${FW_ALL})
  get_filename_component(n ${f} NAME_WE)
  list(FIND SKIP "${n}" i)
  if(i EQUAL -1)
    list(APPEND SRC ${f})
  endif()
endforeach()
foreach(g Arduino_GFX Arduino_G Arduino_DataBus canvas/Arduino_Canvas)
  list(APPEND SRC ${arduino_gfx_SOURCE_DIR}/src/${g}.cpp)
endforeach()

add_library(watchemu SHARED ${SRC})
set_target_properties(watchemu PROPERTIES CXX_STANDARD 17 CXX_EXTENSIONS ON)
target_include_directories(watchemu PRIVATE ${SIM}/shim ${SIM} ${FW} ${arduino_gfx_SOURCE_DIR}/src ${arduinojson_SOURCE_DIR}/src ${jpegdec_SOURCE_DIR}/src)
target_compile_definitions(watchemu PRIVATE ARDUINOJSON_ENABLE_ARDUINO_STRING=0 ARDUINOJSON_ENABLE_ARDUINO_STREAM=0
  ARDUINOJSON_ENABLE_ARDUINO_PRINT=0 EMU_JPEGDEC NO_SIMD)
target_compile_options(watchemu PRIVATE -w -O2 -fvisibility=hidden)
if(ANDROID)
  target_link_libraries(watchemu PRIVATE log)
endif()
target_link_options(watchemu PRIVATE -Wl,--no-undefined)
