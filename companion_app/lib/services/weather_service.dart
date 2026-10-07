import 'dart:convert';
import 'dart:io';
import 'ble_protocol.dart';

/// Current weather + short forecast from Open-Meteo (free, no API key).
class WeatherSnapshot {
  final double tempC;
  final String condition;
  final int code;
  final int humidity;
  final List<WeatherForecast> forecast;
  final DateTime fetchedAt;

  const WeatherSnapshot({
    required this.tempC,
    required this.condition,
    required this.code,
    required this.humidity,
    required this.forecast,
    required this.fetchedAt,
  });

  WeatherData toWatch() => WeatherData(
        tempC: tempC,
        condition: condition,
        humidity: humidity,
        forecast: forecast,
      );
}

class WeatherService {
  static String describe(int code) {
    if (code == 0) return 'Clear';
    if (code <= 2) return 'Partly cloudy';
    if (code == 3) return 'Cloudy';
    if (code == 45 || code == 48) return 'Fog';
    if (code >= 51 && code <= 57) return 'Drizzle';
    if (code >= 61 && code <= 67) return 'Rain';
    if (code >= 71 && code <= 77) return 'Snow';
    if (code >= 80 && code <= 82) return 'Showers';
    if (code >= 85 && code <= 86) return 'Snow showers';
    if (code >= 95) return 'Storm';
    return 'Unknown';
  }

  static const _days = ['Mon', 'Tue', 'Wed', 'Thu', 'Fri', 'Sat', 'Sun'];

  static Future<WeatherSnapshot> fetch(double lat, double lon) async {
    final uri = Uri.parse('https://api.open-meteo.com/v1/forecast'
        '?latitude=${lat.toStringAsFixed(3)}&longitude=${lon.toStringAsFixed(3)}'
        '&current=temperature_2m,relative_humidity_2m,weather_code'
        '&daily=weather_code,temperature_2m_max,temperature_2m_min'
        '&timezone=auto&forecast_days=4');
    final client = HttpClient()..connectionTimeout = const Duration(seconds: 10);
    try {
      final req = await client.getUrl(uri);
      final res = await req.close().timeout(const Duration(seconds: 15));
      final body = await res.transform(utf8.decoder).join();
      if (res.statusCode != 200) {
        throw HttpException('Weather service answered ${res.statusCode}');
      }
      final j = jsonDecode(body) as Map<String, dynamic>;
      final cur = j['current'] as Map<String, dynamic>;
      final code = (cur['weather_code'] as num).toInt();
      final daily = j['daily'] as Map<String, dynamic>;
      final times = (daily['time'] as List).cast<String>();
      final codes = (daily['weather_code'] as List).map((e) => (e as num).toInt()).toList();
      final hi = (daily['temperature_2m_max'] as List).map((e) => (e as num).toDouble()).toList();
      final lo = (daily['temperature_2m_min'] as List).map((e) => (e as num).toDouble()).toList();
      final forecast = <WeatherForecast>[];
      for (var i = 1; i < times.length && i < 4; i++) {
        final d = DateTime.tryParse(times[i]);
        forecast.add(WeatherForecast(
          day: d == null ? times[i] : _days[d.weekday - 1],
          high: hi[i],
          low: lo[i],
          condition: describe(codes[i]),
        ));
      }
      return WeatherSnapshot(
        tempC: (cur['temperature_2m'] as num).toDouble(),
        condition: describe(code),
        code: code,
        humidity: (cur['relative_humidity_2m'] as num).toInt(),
        forecast: forecast,
        fetchedAt: DateTime.now(),
      );
    } finally {
      client.close();
    }
  }
}
