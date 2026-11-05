# The watch emulator for the iPhone app: the firmware's own screen code,
# built against the PC simulator's stubbed hardware (tools/watch_sim), as
# a framework the app opens through dart:ffi (lib/emulator/watch_emulator.dart).
# fetch_libs.sh (run by the Podfile) makes src/ and third_party/.
here = __dir__
list = File.read(File.join(here, 'src', 'sim', 'emu_sources.sh'))
skip = list[/EMU_SKIP=\(([^)]*)\)/m, 1].split
gfx = list[/EMU_GFX=\(([^)]*)\)/m, 1].split

Pod::Spec.new do |s|
  s.name         = 'WatchEmu'
  s.version      = '1.0.0'
  s.summary      = 'The AmoledWatch firmware screens, run on the phone'
  s.homepage     = 'https://github.com/MikeCheek/CustomOS-ESP32-S3-1.75'
  s.license      = { :type => 'Project' }
  s.author       = 'AmoledWatch'
  s.source       = { :path => '.' }
  s.platform     = :ios, '16.0'

  # Every firmware file but the hardware drivers - the same list as the PC
  # simulator and Android (emu_sources.sh), so new files come along.
  s.source_files = ['src/sim/emu_main.cpp', 'src/sim/sim_stubs.cpp', 'src/firmware/*.cpp',
                    'third_party/JPEGDEC/src/JPEGDEC.cpp'] +
                   gfx.map { |g| "third_party/Arduino_GFX/src/#{g}.cpp" }
  s.exclude_files = skip.map { |n| "src/firmware/#{n}.cpp" }
  s.preserve_paths = ['src/**/*.h', 'third_party/**/*.h', 'third_party/**/*.hpp', 'third_party/**/*.inl']
  s.public_header_files = []
  s.libraries = 'c++'

  s.pod_target_xcconfig = {
    # The simulator's shims (Arduino.h, SD.h, Preferences.h...) first, like -I on Android
    'HEADER_SEARCH_PATHS' => [
      '"$(PODS_TARGET_SRCROOT)/src/sim/shim"', '"$(PODS_TARGET_SRCROOT)/src/sim"', '"$(PODS_TARGET_SRCROOT)/src/firmware"',
      '"$(PODS_TARGET_SRCROOT)/third_party/Arduino_GFX/src"', '"$(PODS_TARGET_SRCROOT)/third_party/ArduinoJson/src"',
      '"$(PODS_TARGET_SRCROOT)/third_party/JPEGDEC/src"',
    ].join(' '),
    'USE_HEADERMAP' => 'NO',
    'ALWAYS_SEARCH_USER_PATHS' => 'NO',
    'CLANG_CXX_LANGUAGE_STANDARD' => 'gnu++17',
    'GCC_PREPROCESSOR_DEFINITIONS' => '$(inherited) ARDUINOJSON_ENABLE_ARDUINO_STRING=0 ARDUINOJSON_ENABLE_ARDUINO_STREAM=0 ARDUINOJSON_ENABLE_ARDUINO_PRINT=0 EMU_JPEGDEC NO_SIMD',
    'OTHER_CPLUSPLUSFLAGS' => '$(inherited) -w',
    'GCC_OPTIMIZATION_LEVEL' => '2',
    'GCC_SYMBOLS_PRIVATE_EXTERN' => 'YES',   # only the emu_* functions are exported (visibility("default"))
    'DEFINES_MODULE' => 'NO',
  }
end
