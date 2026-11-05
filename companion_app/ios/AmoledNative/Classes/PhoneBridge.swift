import AVFoundation
import Contacts
import CoreLocation
import EventKit
import Flutter
import UIKit
import UserNotifications

/// "com.amoledwatch.native" on iPhone (lib/services/native_service.dart,
/// PhoneBridge.kt on Android): what iOS lets an app do - battery, contacts,
/// calendar events and reminders, location, find my phone, links, the app
/// version. Reading other apps' notifications, Do Not Disturb, media control
/// of other apps, SMS without asking and installing apps aren't possible on
/// iOS: those answer "nothing" (nil / false) rather than an error.
final class PhoneBridge: NSObject, FlutterStreamHandler {
    private let events = EKEventStore()
    private let location = CLLocationManager()
    private var batterySink: FlutterEventSink?
    private var calendarSink: FlutterEventSink?
    private var finder: AVAudioPlayer?
    private let smsTaps = SmsTapHandler()

    init(messenger: FlutterBinaryMessenger) {
        super.init()
        UIDevice.current.isBatteryMonitoringEnabled = true
        smsTaps.install()
        FlutterMethodChannel(name: "com.amoledwatch.native", binaryMessenger: messenger)
            .setMethodCallHandler { [weak self] call, result in self?.handle(call, result) }
        FlutterEventChannel(name: "com.amoledwatch.native/battery", binaryMessenger: messenger).setStreamHandler(self)
        FlutterEventChannel(name: "com.amoledwatch.native/calendar", binaryMessenger: messenger)
            .setStreamHandler(CalendarStream(owner: self))
        // Android-only streams: quiet on iOS, instead of "no implementation" errors
        for n in ["notifications", "media", "navigation", "widget", "dnd"] {
            FlutterEventChannel(name: "com.amoledwatch.native/\(n)", binaryMessenger: messenger)
                .setStreamHandler(SilentStream())
        }
        NotificationCenter.default.addObserver(self, selector: #selector(batteryChanged),
                                               name: UIDevice.batteryLevelDidChangeNotification, object: nil)
        NotificationCenter.default.addObserver(self, selector: #selector(batteryChanged),
                                               name: UIDevice.batteryStateDidChangeNotification, object: nil)
        NotificationCenter.default.addObserver(self, selector: #selector(calendarChanged),
                                               name: .EKEventStoreChanged, object: events)
    }

    private func handle(_ call: FlutterMethodCall, _ result: @escaping FlutterResult) {
        let args = call.arguments as? [String: Any]
        switch call.method {
        case "getBattery": result(battery())
        case "getAppVersion":
            result(Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String)
        case "openUrl":
            guard let s = args?["url"] as? String, let url = URL(string: s) else { result(false); return }
            UIApplication.shared.open(url) { ok in result(ok) }
        case "getContacts": contacts(result)
        case "getEvents": result(upcoming(hours: args?["hours"] as? Int ?? 48))
        case "addReminder":
            let ms = (args?["begin"] as? NSNumber)?.int64Value ?? 0
            result(addReminder(title: args?["title"] as? String ?? "Reminder", at: ms))
        case "findPhone": findPhone(args?["on"] as? Bool ?? false); result(true)
        case "getLocation":
            if let l = location.location?.coordinate { result(["lat": l.latitude, "lon": l.longitude]) } else { result(nil) }
        case "watchCalendar": result(true)
        case "isNotificationListenerEnabled", "canInstallApks", "notificationAction", "mediaCommand", "setDnd",
             "installApk", "startLinkService":
            result(false)
        case "sendSms":
            // iOS doesn't let an app send a text by itself: open Messages with it written
            // (app in front), or post a notification that does when tapped (fall alerts).
            result(composeText(to: args?["to"] as? String ?? "", body: args?["text"] as? String ?? ""))
        default:
            result(nil)   // Android-only settings and helpers: nothing to do here
        }
    }

    // ---- battery ----
    private func battery() -> [String: Any] {
        let d = UIDevice.current
        let level = d.batteryLevel < 0 ? -1 : Int((d.batteryLevel * 100).rounded())
        return ["level": level, "charging": d.batteryState == .charging || d.batteryState == .full]
    }

    @objc private func batteryChanged() { batterySink?(battery()) }

    func onListen(withArguments arguments: Any?, eventSink events: @escaping FlutterEventSink) -> FlutterError? {
        batterySink = events
        events(battery())
        return nil
    }

    func onCancel(withArguments arguments: Any?) -> FlutterError? {
        batterySink = nil
        return nil
    }

    // ---- calendar ----
    fileprivate func setCalendarSink(_ s: FlutterEventSink?) { calendarSink = s }
    @objc private func calendarChanged() { calendarSink?("changed") }

    private var calendarAllowed: Bool {
        let st = EKEventStore.authorizationStatus(for: .event)
        if #available(iOS 17.0, *) { return st == .fullAccess }
        return st == .authorized
    }

    /// [{id, title, begin, end (ms UTC), allDay, location, color 0xRRGGBB}], from an hour ago.
    private func upcoming(hours: Int) -> [[String: Any]] {
        guard calendarAllowed else { return [] }
        let now = Date()
        let pred = events.predicateForEvents(withStart: now.addingTimeInterval(-3600),
                                             end: now.addingTimeInterval(Double(hours) * 3600), calendars: nil)
        return events.events(matching: pred).map { e in
            [
                "id": stableId(e.eventIdentifier ?? e.title ?? "", e.startDate),
                "title": e.title ?? "",
                "begin": Int64(e.startDate.timeIntervalSince1970 * 1000),
                "end": Int64(e.endDate.timeIntervalSince1970 * 1000),
                "allDay": e.isAllDay,
                "location": e.location ?? "",
                "color": rgb(e.calendar.cgColor),
            ]
        }
    }

    /// A short event with an alert, in the default calendar: its id, or -1.
    private func addReminder(title: String, at ms: Int64) -> Int64 {
        guard calendarAllowed, let cal = events.defaultCalendarForNewEvents else { return -1 }
        let start = Date(timeIntervalSince1970: Double(ms) / 1000)
        let e = EKEvent(eventStore: events)
        e.title = title
        e.startDate = start
        e.endDate = start.addingTimeInterval(15 * 60)
        e.calendar = cal
        e.addAlarm(EKAlarm(relativeOffset: 0))
        do { try events.save(e, span: .thisEvent) } catch { return -1 }
        return stableId(e.eventIdentifier ?? title, start)
    }

    // Event ids are strings on iOS; the app wants a number that's the same every time.
    private func stableId(_ s: String, _ d: Date) -> Int64 {
        var h: UInt64 = 1469598103934665603
        for b in "\(s)@\(Int64(d.timeIntervalSince1970))".utf8 { h = (h ^ UInt64(b)) &* 1099511628211 }
        return Int64(bitPattern: h & 0x3FFF_FFFF_FFFF_FFFF)
    }

    private func rgb(_ c: CGColor?) -> Int {
        guard let c = c, let comp = c.converted(to: CGColorSpaceCreateDeviceRGB(), intent: .defaultIntent, options: nil)?.components,
              comp.count >= 3 else { return 0x7B61FF }
        return (Int(comp[0] * 255) << 16) | (Int(comp[1] * 255) << 8) | Int(comp[2] * 255)
    }

    // ---- contacts: [{name, phone}] ----
    private func contacts(_ result: @escaping FlutterResult) {
        let store = CNContactStore()
        store.requestAccess(for: .contacts) { ok, _ in
            guard ok else {
                DispatchQueue.main.async { result(FlutterError(code: "permission", message: "Contacts not allowed", details: nil)) }
                return
            }
            DispatchQueue.global(qos: .userInitiated).async {
                var out: [[String: String]] = []
                let keys = [CNContactGivenNameKey, CNContactFamilyNameKey, CNContactPhoneNumbersKey] as [CNKeyDescriptor]
                let req = CNContactFetchRequest(keysToFetch: keys)
                req.sortOrder = .userDefault
                try? store.enumerateContacts(with: req) { c, _ in
                    let name = [c.givenName, c.familyName].filter { !$0.isEmpty }.joined(separator: " ")
                    for p in c.phoneNumbers { out.append(["name": name, "phone": p.value.stringValue]) }
                }
                DispatchQueue.main.async { result(out) }
            }
        }
    }

    // ---- texts: Messages with the text ready ----
    static func smsUrl(to: String, body: String) -> URL? {
        let number = to.filter { "+0123456789".contains($0) }
        guard !number.isEmpty else { return nil }
        var allowed = CharacterSet.urlQueryAllowed
        allowed.remove(charactersIn: "&=?+")
        let text = body.addingPercentEncoding(withAllowedCharacters: allowed) ?? ""
        return URL(string: "sms:\(number)&body=\(text)")   // iOS's own sms: form
    }

    private func composeText(to: String, body: String) -> Bool {
        guard let url = PhoneBridge.smsUrl(to: to, body: body) else { return false }
        if UIApplication.shared.applicationState == .active {
            UIApplication.shared.open(url)
            return true
        }
        let content = UNMutableNotificationContent()
        content.title = "Fall alert"
        content.body = "Tap to send your emergency contact the text with your location"
        content.sound = .default
        content.interruptionLevel = .timeSensitive
        content.userInfo = ["sms": url.absoluteString]
        UNUserNotificationCenter.current().add(
            UNNotificationRequest(identifier: "amoled.fall", content: content, trigger: nil))
        return true
    }

    // ---- find my phone: a loud repeating beep until stopped ----
    private func findPhone(_ on: Bool) {
        finder?.stop()
        finder = nil
        guard on else { return }
        AudioSession.activate()
        let rate = 22050
        var pcm = Data()
        for i in 0..<(rate / 2) {   // 0.5 s: two short beeps, then quiet
            let t = Double(i) / Double(rate)
            let on = (t < 0.12) || (t > 0.18 && t < 0.30)
            var v = Int16(on ? sin(2 * .pi * 1760 * t) * 26000 : 0).littleEndian
            pcm.append(Data(bytes: &v, count: 2))
        }
        var wav = MemoRecorder.wavHeader(dataBytes: pcm.count, rate: rate)
        wav.append(pcm)
        finder = try? AVAudioPlayer(data: wav)
        finder?.numberOfLoops = -1
        finder?.volume = 1
        finder?.play()
    }
}

/// Taps on the fall alert's notification open Messages with the text; any
/// other notification goes on to the delegate that was there before.
final class SmsTapHandler: NSObject, UNUserNotificationCenterDelegate {
    private weak var next: UNUserNotificationCenterDelegate?

    func install() {
        let c = UNUserNotificationCenter.current()
        if c.delegate !== self {
            next = c.delegate
            c.delegate = self
        }
    }

    func userNotificationCenter(_ center: UNUserNotificationCenter, didReceive response: UNNotificationResponse,
                                withCompletionHandler done: @escaping () -> Void) {
        if let s = response.notification.request.content.userInfo["sms"] as? String, let url = URL(string: s) {
            UIApplication.shared.open(url)
            done()
        } else if let n = next {
            n.userNotificationCenter?(center, didReceive: response, withCompletionHandler: done) ?? done()
        } else {
            done()
        }
    }

    func userNotificationCenter(_ center: UNUserNotificationCenter, willPresent notification: UNNotification,
                                withCompletionHandler done: @escaping (UNNotificationPresentationOptions) -> Void) {
        if let n = next, n.responds(to: #selector(UNUserNotificationCenterDelegate.userNotificationCenter(_:willPresent:withCompletionHandler:))) {
            n.userNotificationCenter?(center, willPresent: notification, withCompletionHandler: done)
        } else {
            done([.banner, .sound])
        }
    }
}

private final class CalendarStream: NSObject, FlutterStreamHandler {
    private weak var owner: PhoneBridge?
    init(owner: PhoneBridge) { self.owner = owner }
    func onListen(withArguments arguments: Any?, eventSink events: @escaping FlutterEventSink) -> FlutterError? {
        owner?.setCalendarSink(events)
        return nil
    }
    func onCancel(withArguments arguments: Any?) -> FlutterError? {
        owner?.setCalendarSink(nil)
        return nil
    }
}

/// A stream that never sends anything (features iOS doesn't have).
final class SilentStream: NSObject, FlutterStreamHandler {
    func onListen(withArguments arguments: Any?, eventSink events: @escaping FlutterEventSink) -> FlutterError? { nil }
    func onCancel(withArguments arguments: Any?) -> FlutterError? { nil }
}
