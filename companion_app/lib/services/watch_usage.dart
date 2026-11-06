import 'dart:math' as math;

/// How the watch is used, as it reports it ({"e":"usage"}, three parts -
/// firmware/usage_stats.h), and what that says: drain with the screen on and
/// off, battery life, the busiest hours, where the battery goes, problems to
/// look at. Plain Dart, so it's tested on the PC.
class WatchUsage {
  /// Battery % every [intervalMin] minutes, oldest first (null: no sample).
  final List<int?> battery;
  final List<bool> charging;
  final int intervalMin;
  final DateTime at; // when the report came (the last sample is about now)
  final List<int> onToday, wakesToday, onYesterday; // per hour: screen-on seconds, wake-ups
  final List<(String, int)> apps; // seconds on screen today, most first
  final int wakes, disconnects, notifications, boots, crashes;
  final int minHeapKb, psramKb, uptimeS;
  final int? sdTotalMb, sdUsedMb;
  final String resetReason;

  /// What kept the watch from sleeping deeply with the screen off today,
  /// seconds each (firmware 3.5+, part 3: [awakeLabels] keys), and the
  /// seconds it was off; [hasPowerManagement] false: a build without it.
  final Map<String, int> awake;
  final int offS;
  final bool hasPowerManagement;

  /// Seconds today each costly part was in use, unplugged (part 3 "ft":
  /// speaker, mic, gps, wifi - firmware 3.5+).
  final Map<String, int> features;

  /// The last 7 days as the watch kept them (part 4, newest first): date,
  /// screen-on minutes, wake-ups, battery % used unplugged, charges.
  final List<UsageDay> week;

  const WatchUsage({
    required this.battery,
    required this.charging,
    required this.intervalMin,
    required this.at,
    required this.onToday,
    required this.wakesToday,
    required this.onYesterday,
    required this.apps,
    this.wakes = 0,
    this.disconnects = 0,
    this.notifications = 0,
    this.boots = 0,
    this.crashes = 0,
    this.minHeapKb = 0,
    this.psramKb = 0,
    this.uptimeS = 0,
    this.sdTotalMb,
    this.sdUsedMb,
    this.resetReason = '',
    this.awake = const {},
    this.offS = 0,
    this.hasPowerManagement = true,
    this.features = const {},
    this.week = const [],
  });

  /// Puts the parts together (in any order); null until the first three
  /// are here (the fourth, what kept it awake, comes from newer firmware).
  static WatchUsage? fromParts(Map<int, Map<String, dynamic>> p, DateTime at) {
    final a = p[0], b = p[1], c = p[2], d = p[3], wk = p[4];
    if (a == null || b == null || c == null) return null;
    final hex = a['bat'] as String? ?? '';
    final bat = <int?>[
      for (var i = 0; i + 1 < hex.length; i += 2)
        () {
          final v = int.tryParse(hex.substring(i, i + 2), radix: 16) ?? 255;
          return v > 100 ? null : v;
        }(),
    ];
    final chgS = a['chg'] as String? ?? '';
    List<int> ints(dynamic l) => [for (final x in (l as List? ?? const [])) (x as num).toInt()];
    final sd = c['sd'] as List?;
    return WatchUsage(
      battery: bat,
      charging: [for (var i = 0; i < bat.length; i++) i < chgS.length && chgS[i] == '1'],
      intervalMin: (a['iv'] as num?)?.toInt() ?? 10,
      at: at,
      onToday: ints(b['on']),
      wakesToday: ints(b['wk']),
      onYesterday: ints(b['ony']),
      apps: [
        for (final x in (c['apps'] as List? ?? const []))
          if (x is List && x.length >= 2) ('${x[0]}', (x[1] as num).toInt()),
      ],
      wakes: (c['wake'] as num?)?.toInt() ?? 0,
      disconnects: (c['disc'] as num?)?.toInt() ?? 0,
      notifications: (c['ntf'] as num?)?.toInt() ?? 0,
      boots: (c['boot'] as num?)?.toInt() ?? 0,
      crashes: (c['crash'] as num?)?.toInt() ?? 0,
      minHeapKb: (c['heap'] as num?)?.toInt() ?? 0,
      psramKb: (c['psram'] as num?)?.toInt() ?? 0,
      uptimeS: (c['up'] as num?)?.toInt() ?? 0,
      sdTotalMb: sd != null && sd.length >= 2 ? (sd[0] as num).toInt() : null,
      sdUsedMb: sd != null && sd.length >= 2 ? (sd[1] as num).toInt() : null,
      resetReason: c['rr'] as String? ?? '',
      awake: {
        for (final e in ((d?['aw'] as Map?) ?? const {}).entries)
          if (e.value is num && (e.value as num) > 0) '${e.key}': (e.value as num).toInt(),
      },
      offS: (d?['off'] as num?)?.toInt() ?? 0,
      hasPowerManagement: (d?['pm'] as num?)?.toInt() != 0,
      features: {
        for (final e in ((d?['ft'] as Map?) ?? const {}).entries)
          if (e.value is num && (e.value as num) > 0) '${e.key}': (e.value as num).toInt(),
      },
      week: [
        for (final r in (wk?['wk'] as List? ?? const []))
          if (r is List && r.length >= 5)
            UsageDay(
              DateTime((r[0] as num).toInt() ~/ 10000, (r[0] as num).toInt() ~/ 100 % 100, (r[0] as num).toInt() % 100),
              (r[1] as num).toInt(),
              (r[2] as num).toInt(),
              (r[3] as num).toInt(),
              (r[4] as num).toInt(),
            ),
      ],
    );
  }

  /// When sample [i] was taken.
  DateTime sampleTime(int i) => at.subtract(Duration(minutes: (battery.length - 1 - i) * intervalMin));

  /// Screen-on seconds in the hour that started at [t] (today's or yesterday's list).
  int _onAt(DateTime t) {
    final today = DateTime(at.year, at.month, at.day);
    final list = t.isBefore(today) ? onYesterday : onToday;
    return list.length == 24 ? list[t.hour] : 0;
  }

  /// Drain in %/hour with the screen on and off, from the battery's fall in
  /// each hour against that hour's screen-on time (least squares on
  /// drop = on * hoursOn + off * hoursOff). Null without enough unplugged data.
  ({double on, double off})? drainRates() {
    // per clock hour: battery dropped, and how long it was unplugged
    final drop = <DateTime, double>{}, span = <DateTime, double>{};
    for (var i = 1; i < battery.length; i++) {
      final a = battery[i - 1], b = battery[i];
      if (a == null || b == null || charging[i] || charging[i - 1] || b > a) continue;
      final t = sampleTime(i);
      final h = DateTime(t.year, t.month, t.day, t.hour);
      drop[h] = (drop[h] ?? 0) + (a - b);
      span[h] = (span[h] ?? 0) + intervalMin / 60;
    }
    final hours = [
      for (final h in drop.keys)
        if ((span[h] ?? 0) >= 0.5) h,
    ];
    if (hours.length < 4) return null;
    // normal equations for two unknowns
    double s11 = 0, s12 = 0, s22 = 0, r1 = 0, r2 = 0;
    for (final h in hours) {
      final total = span[h]!;
      final on = math.min(_onAt(h) / 3600, total);
      final off = total - on;
      s11 += on * on;
      s12 += on * off;
      s22 += off * off;
      r1 += on * drop[h]!;
      r2 += off * drop[h]!;
    }
    final det = s11 * s22 - s12 * s12;
    final avg = hours.fold(0.0, (x, h) => x + drop[h]!) / hours.fold(0.0, (x, h) => x + span[h]!);
    if (det.abs() < 1e-6) return (on: avg, off: avg); // no spread in screen time: just the average
    var on = (r1 * s22 - r2 * s12) / det, off = (s11 * r2 - s12 * r1) / det;
    off = off.clamp(0.0, 50.0);
    on = on.clamp(off, 100.0);
    return (on: on, off: off);
  }

  /// The average drain over the unplugged part of the day, %/hour.
  double? averageDrain() {
    double d = 0, h = 0;
    for (var i = 1; i < battery.length; i++) {
      final a = battery[i - 1], b = battery[i];
      if (a == null || b == null || charging[i] || charging[i - 1] || b > a) continue;
      d += a - b;
      h += intervalMin / 60;
    }
    return h >= 2 ? d / h : null;
  }

  /// Hours a full battery lasts at today's pace.
  double? fullChargeHours() {
    final r = averageDrain();
    return r == null || r <= 0.05 ? null : 100 / r;
  }

  int chargesToday() {
    var n = 0;
    for (var i = 1; i < charging.length; i++) {
      if (charging[i] && !charging[i - 1]) n++;
    }
    return n;
  }

  int get screenOnTodayS => onToday.fold(0, (a, b) => a + b);

  /// The busiest hours today: (hour, seconds on), most first.
  List<(int, int)> peakHours([int n = 3]) {
    final l = [
      for (var h = 0; h < onToday.length; h++)
        if (onToday[h] > 0) (h, onToday[h]),
    ];
    l.sort((a, b) => b.$2.compareTo(a.$2));
    return l.take(n).toList();
  }

  /// What each costly part took of the battery today, estimated: the time it
  /// was on (as the watch counted it) at its typical extra draw, for a
  /// [batteryMah] cell; with the screen measured from the drain for scale.
  /// (label, what uses it, seconds, % of a full battery), most first.
  List<(String, String, int, double)> featureCosts({int batteryMah = 350}) {
    const mA = {'speaker': 45.0, 'mic': 20.0, 'gps': 30.0, 'wifi': 60.0};
    const what = {
      'speaker': ('Speaker', 'spoken answers, music, games'),
      'mic': ('Microphone', 'the assistant, memos, voice replies'),
      'gps': ('GPS', 'workouts, maps'),
      'wifi': ('Wi-Fi', 'updates, transfers'),
    };
    final out = <(String, String, int, double)>[
      for (final e in features.entries)
        if (mA[e.key] != null)
          (what[e.key]!.$1, what[e.key]!.$2, e.value, e.value / 3600 * mA[e.key]! / batteryMah * 100),
    ];
    final rates = drainRates();
    if (rates != null && screenOnTodayS > 0) {
      out.add(('Screen on', 'measured from the drain', screenOnTodayS, rates.on * screenOnTodayS / 3600));
    }
    out.sort((a, b) => b.$4.compareTo(a.$4));
    return out;
  }

  /// Where today's battery went, % of what was used: each app by its screen
  /// time at the screen-on rate, and the rest as standby.
  List<(String, double)> drainShares() {
    final rates = drainRates();
    if (rates == null) return const [];
    final onH = screenOnTodayS / 3600;
    final hoursSoFar = at.hour + at.minute / 60;
    final offH = math.max(0.0, hoursSoFar - onH);
    final screen = rates.on * onH, standby = rates.off * offH;
    final total = screen + standby;
    if (total <= 0) return const [];
    final appS = apps.fold(0, (a, b) => a + b.$2);
    final out = <(String, double)>[
      for (final (name, s) in apps)
        if (appS > 0) (name, 100 * screen * s / appS / total),
      ('Standby (screen off)', 100 * standby / total),
    ]..sort((a, b) => b.$2.compareTo(a.$2));
    return out;
  }
}

/// One past day on the watch (usage part 4).
class UsageDay {
  final DateTime date;
  final int onMin, wakes, dropPct, charges;
  const UsageDay(this.date, this.onMin, this.wakes, this.dropPct, this.charges);
}

enum Severity { ok, info, warn, bad }

/// What kept the watch awake with the screen off (firmware usage_stats.h):
/// a name, and what to do about it.
const awakeLabels = <String, (String, String)>{
  'build': (
    'This firmware',
    'It has no deep sleep: install the release firmware (Settings > Software update, or GitHub)',
  ),
  'setting': ('Deep sleep being off', 'Turn on Settings > Battery > Deep sleep on the watch'),
  'wakeword': ('The wake word', 'Settings > Voice > Screen off too: off, so it listens only with the screen on'),
  'aod': ('Always-on', 'Turn off the always-on clock (Faces tab, or Settings > Display)'),
  'gps': ('GPS', 'Turn GPS off when you\'re not recording a workout (Settings > Connections)'),
  'wifi': ('Wi-Fi', 'Turn Wi-Fi off when you don\'t need it (Control Center)'),
  'espnow': ('ESP-NOW', 'Turn ESP-NOW off (Settings > Connections)'),
  'audio': ('Music on the watch', 'Playing on the watch keeps it awake - expected while you listen'),
  'usbmode': ('A USB mode', 'Switch the USB mode back to normal (Settings > System > USB)'),
};

class Finding {
  final Severity severity;
  final String title, detail;
  const Finding(this.severity, this.title, this.detail);
}

/// Things worth knowing from the report: problems first, then upkeep.
List<Finding> usageFindings(WatchUsage u, {bool crashReport = false, bool firmwareUpdate = false, String? firmware}) {
  final f = <Finding>[];
  if (crashReport)
    f.add(
      const Finding(Severity.bad, 'A crash report is waiting', 'See "Last crash" below and send it with a bug report'),
    );
  final rr = u.resetReason.toLowerCase();
  if (rr.contains('panic') || rr.contains('watchdog') || rr.contains('brownout') || rr.contains('exception')) {
    f.add(
      Finding(
        Severity.bad,
        'The last restart wasn\'t clean',
        'Reason: ${u.resetReason}. If it repeats, update the firmware',
      ),
    );
  }
  // what kept it awake with the screen off: anything over a fifth of that time
  if (u.offS >= 1800) {
    final top = u.awake.entries.where((e) => e.value >= u.offS / 5).toList()
      ..sort((a, b) => b.value.compareTo(a.value));
    for (final e in top) {
      final info = awakeLabels[e.key];
      if (info == null) continue;
      final h = (e.value / 3600).toStringAsFixed(1);
      f.add(Finding(Severity.warn, '${info.$1} kept it awake $h h today', info.$2));
    }
  }
  final r = u.drainRates();
  if (r != null && r.off > 2.0 && u.awake.isEmpty) {
    f.add(
      Finding(
        Severity.warn,
        'High drain with the screen off (${r.off.toStringAsFixed(1)}%/h)',
        'Something keeps the watch awake: the wake word, Wi-Fi, GPS or always-on. Low Power Mode turns them off',
      ),
    );
  }
  if (r != null && r.on > 25) {
    f.add(
      Finding(
        Severity.warn,
        'High drain with the screen on (${r.on.toStringAsFixed(0)}%/h)',
        'Lower the brightness or use the Balanced battery mode',
      ),
    );
  }
  final peaks = u.peakHours(1);
  if (u.screenOnTodayS > 3 * 3600) {
    f.add(
      Finding(
        Severity.info,
        'Lots of screen time today (${(u.screenOnTodayS / 3600).toStringAsFixed(1)} h)',
        'A shorter screen timeout or raise to wake saves the most battery',
      ),
    );
  }
  if (u.wakes > 250) {
    f.add(
      Finding(
        Severity.info,
        '${u.wakes} wake-ups today',
        'If many are by accident, turn off Wake on Touch or Wake on Motion',
      ),
    );
  }
  if (u.disconnects > 10) {
    f.add(
      Finding(
        Severity.warn,
        'Bluetooth dropped ${u.disconnects} times today',
        'Keep the phone closer, or check its battery saver isn\'t closing the app',
      ),
    );
  }
  if (u.minHeapKb > 0 && u.minHeapKb < 24) {
    f.add(
      Finding(
        Severity.warn,
        'Memory ran low (${u.minHeapKb} KB free at worst)',
        'A restart frees it; tell us if it comes back',
      ),
    );
  }
  if (u.sdTotalMb != null && u.sdUsedMb != null && u.sdTotalMb! > 0) {
    final freePct = 100 * (u.sdTotalMb! - u.sdUsedMb!) / u.sdTotalMb!;
    if (freePct < 10) {
      f.add(
        Finding(
          Severity.warn,
          'The SD card is almost full (${freePct.round()}% free)',
          'Delete old recordings or videos (Files on the watch, or Send files in the app)',
        ),
      );
    }
  }
  final low = u.battery.where((b) => b != null && b <= 5).length;
  if (low > 0) {
    f.add(
      const Finding(
        Severity.info,
        'The battery got very low today',
        'Running empty often wears a battery faster: charge at 20% or so',
      ),
    );
  }
  if (u.chargesToday() >= 3) {
    f.add(
      Finding(
        Severity.info,
        'Charged ${u.chargesToday()} times in 24 hours',
        'Fine for the battery - but see what uses it below',
      ),
    );
  }
  if (firmwareUpdate) f.add(const Finding(Severity.info, 'A firmware update is out', 'Settings > Software update'));
  if (f.isEmpty) {
    f.add(
      Finding(
        Severity.ok,
        'Everything looks fine',
        peaks.isEmpty ? 'No problems in the last day' : 'Busiest hour: ${peaks.first.$1}:00',
      ),
    );
  }
  return f;
}
