import Flutter

/// Registers the app's own platform channels on iPhone (AppDelegate.swift).
public final class AmoledNative {
    private static var keep: [AnyObject] = []   // the handlers live as long as the app

    public static func register(messenger: FlutterBinaryMessenger) {
        AudioSession.activate()
        keep = [
            MemoRecorder(messenger: messenger),
            MemoPlayer(messenger: messenger),
            PhoneBridge(messenger: messenger),
            EmuIo(messenger: messenger),
            Segmenter(messenger: messenger),
        ]
    }
}
