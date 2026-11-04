/// Between the watch's camera remote (firmware/app_camera.h) and the app's
/// camera screen: the watch's buttons ({"e":"cam","a":...}) reach the screen
/// while it's open; "open" from the watch opens it.
class CameraRemote {
  CameraRemote._();
  static final instance = CameraRemote._();

  /// The camera screen is showing (it owns the camera then).
  bool screenOpen = false;

  /// Another app has the camera (Android tells): the watch was told.
  bool externalOpen = false;

  /// Set by the camera screen while it's open.
  void Function(String action)? onAction;

  /// Opens the camera screen (set by the app, through its navigator).
  void Function()? openScreen;

  void onWatch(Map<String, dynamic> e) {
    final a = e['a'] as String? ?? '';
    if (a == 'open') {
      if (!screenOpen) openScreen?.call();
      return;
    }
    onAction?.call(a);
  }
}
