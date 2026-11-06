import 'package:amoled_companion/services/watch_usage.dart';
import 'package:flutter_test/flutter_test.dart';

/// A day: 2 %/h with the screen off, 20 %/h with it on, some hours busy.
WatchUsage _day() {
  final at = DateTime(2026, 11, 4, 20, 0);
  final on = List<int>.filled(24, 0);
  for (final h in [8, 12, 13, 18]) {
    on[h] = 1800; // half the hour on screen
  }
  // battery every 10 min over the last 24 h, worked out from those rates
  final bat = <int?>[];
  var level = 100.0;
  for (var i = 0; i < 144; i++) {
    final t = at.subtract(Duration(minutes: (143 - i) * 10));
    final today = t.day == at.day;
    final f = today ? on[t.hour] / 3600 : 0.0;
    level -= (f * 20 + (1 - f) * 2) / 6;
    bat.add(level.round().clamp(0, 100));
  }
  return WatchUsage(
    battery: bat,
    charging: List.filled(144, false),
    intervalMin: 10,
    at: at,
    onToday: on,
    wakesToday: List.filled(24, 0),
    onYesterday: List.filled(24, 0),
    apps: const [('Watch face', 3600), ('Music', 1800), ('Workout', 1800)],
    wakes: 40,
  );
}

void main() {
  test('the week from part 4', () {
    final u = WatchUsage.fromParts({
      0: {'bat': 'ff' * 144, 'chg': '0' * 144, 'iv': 10},
      1: {'on': List.filled(24, 0), 'wk': List.filled(24, 0), 'ony': List.filled(24, 0)},
      2: {'apps': [], 'rr': 'power on'},
      4: {
        'wk': [
          [20261103, 95, 120, 38, 1],
          [20261102, 60, 80, 25, 0],
        ],
      },
    }, DateTime(2026, 11, 4))!;
    expect(u.week.length, 2);
    expect(u.week.first.date, DateTime(2026, 11, 3));
    expect(u.week.first.dropPct, 38);
  });

  test('what kept it awake: the fourth part, named with a fix', () {
    final base = {
      0: {'bat': 'ff' * 144, 'chg': '0' * 144, 'iv': 10},
      1: {'on': List.filled(24, 0), 'wk': List.filled(24, 0), 'ony': List.filled(24, 0)},
      2: {'apps': [], 'rr': 'power on'},
    };
    expect(WatchUsage.fromParts(base, DateTime(2026, 11, 4))!.awake, isEmpty);
    final u = WatchUsage.fromParts({
      ...base,
      3: {
        'off': 36000,
        'pm': 1,
        'aw': {'wakeword': 30000, 'gps': 600},
      },
    }, DateTime(2026, 11, 4))!;
    expect(u.awake['wakeword'], 30000);
    final titles = usageFindings(u).map((f) => f.title).toList();
    expect(titles.any((t) => t.startsWith('The wake word kept it awake 8.3 h')), isTrue);
    expect(titles.any((t) => t.startsWith('GPS')), isFalse); // under a fifth of the time
  });

  test('battery cost per feature: time on at its typical draw', () {
    final u = WatchUsage.fromParts({
      0: {'bat': 'ff' * 144, 'chg': '0' * 144, 'iv': 10},
      1: {'on': List.filled(24, 0), 'wk': List.filled(24, 0), 'ony': List.filled(24, 0)},
      2: {'apps': [], 'rr': 'power on'},
      3: {
        'off': 3600,
        'pm': 1,
        'ft': {'speaker': 1800, 'gps': 3600, 'mic': 0},
      },
    }, DateTime(2026, 11, 4))!;
    final c = u.featureCosts(batteryMah: 300);
    expect(c.map((x) => x.$1).toList(), ['GPS', 'Speaker']); // most first; unused ones left out
    expect(c.first.$4, closeTo(10, 0.01)); // an hour at 30 mA of 300 mAh
    expect(c.last.$4, closeTo(7.5, 0.01)); // half an hour at 45 mA
  });

  test('the three parts, put together', () {
    final u = WatchUsage.fromParts({
      2: {
        'apps': [
          ['Music', 120],
        ],
        'wake': 3,
        'sd': [30000, 29000],
        'rr': 'Power on',
      },
      0: {'iv': 10, 'bat': '5251ff', 'chg': '001'},
      1: {'on': List.filled(24, 0), 'wk': List.filled(24, 0), 'ony': List.filled(24, 0)},
    }, DateTime(2026, 11, 4, 12))!;
    expect(u.battery, [82, 81, null]);
    expect(u.charging, [false, false, true]);
    expect(u.apps.single.$1, 'Music');
    expect(WatchUsage.fromParts({0: {}, 1: {}}, DateTime.now()), isNull);
  });

  test('screen-on and screen-off drain come apart', () {
    final r = _day().drainRates()!;
    expect(r.off, closeTo(2, 0.8));
    expect(r.on, closeTo(20, 6));
    expect(_day().fullChargeHours(), isNotNull);
  });

  test('peak hours, shares, findings', () {
    final u = _day();
    expect(u.peakHours().every((p) => {8, 12, 13, 18}.contains(p.$1)), isTrue); // the busy hours (they tie)
    expect(u.peakHours().length, 3);
    final shares = u.drainShares();
    expect(shares.fold(0.0, (a, s) => a + s.$2), closeTo(100, 0.5));
    expect(shares.any((s) => s.$1.startsWith('Standby')), isTrue);
    final f = usageFindings(u, firmwareUpdate: true);
    expect(f.any((x) => x.title.contains('firmware update')), isTrue);
    expect(usageFindings(u.copyFull(disconnects: 30)).any((x) => x.title.contains('Bluetooth')), isTrue);
  });
}

extension on WatchUsage {
  WatchUsage copyFull({int? disconnects}) => WatchUsage(
    battery: battery,
    charging: charging,
    intervalMin: intervalMin,
    at: at,
    onToday: onToday,
    wakesToday: wakesToday,
    onYesterday: onYesterday,
    apps: apps,
    disconnects: disconnects ?? this.disconnects,
  );
}
