# The iPhone side of the app's own platform channels - the Swift twins of
# the Kotlin in android/app/src/main/kotlin (MemoRecorder, MemoPlayer,
# PhoneBridge, EmuIo) and Vision subject masks for photo faces.
# AppDelegate.swift calls AmoledNative.register().
Pod::Spec.new do |s|
  s.name         = 'AmoledNative'
  s.version      = '1.0.0'
  s.summary      = 'AmoledWatch companion: recorder, player, phone bridge, emulator I/O, subject masks'
  s.homepage     = 'https://github.com/MikeCheek/CustomOS-ESP32-S3-Amoled'
  s.license      = { :type => 'Project' }
  s.author       = 'AmoledWatch'
  s.source       = { :path => '.' }
  s.platform     = :ios, '16.0'
  s.swift_version = '5.0'
  s.source_files = 'Classes/**/*.swift'
  s.dependency 'Flutter'
  s.frameworks   = 'AVFoundation', 'Contacts', 'EventKit', 'CoreLocation', 'CoreMotion', 'Vision'
end
