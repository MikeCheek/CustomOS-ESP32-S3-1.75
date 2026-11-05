import AmoledNative
import Flutter
import UIKit

@main
@objc class AppDelegate: FlutterAppDelegate {
  override func application(
    _ application: UIApplication,
    didFinishLaunchingWithOptions launchOptions: [UIApplication.LaunchOptionsKey: Any]?
  ) -> Bool {
    GeneratedPluginRegistrant.register(with: self)
    // The app's own channels: recorder, player, phone bridge, emulator I/O,
    // subject masks (ios/AmoledNative)
    if let registrar = self.registrar(forPlugin: "AmoledNative") {
      AmoledNative.register(messenger: registrar.messenger())
    }
    return super.application(application, didFinishLaunchingWithOptions: launchOptions)
  }
}
