import 'package:shared_preferences/shared_preferences.dart';

/// Fall alerts (watch: app_fall.cpp): who gets a text when the watch asked
/// "Did you fall?" and nobody answered.
class Safety {
  static const _nameKey = 'sos_name', _phoneKey = 'sos_phone';

  static Future<({String name, String phone})> contact() async {
    final p = await SharedPreferences.getInstance();
    return (name: p.getString(_nameKey) ?? '', phone: p.getString(_phoneKey) ?? '');
  }

  static Future<void> setContact(String name, String phone) async {
    final p = await SharedPreferences.getInstance();
    await p.setString(_nameKey, name.trim());
    await p.setString(_phoneKey, phone.replaceAll(RegExp(r'[^\d+]'), ''));
  }

  /// The text the contact gets. [lat]/[lon] when the phone knows where it is.
  static String alertText(DateTime at, {double? lat, double? lon, bool test = false, bool sos = false}) {
    final hh = at.hour.toString().padLeft(2, '0'), mm = at.minute.toString().padLeft(2, '0');
    final where = lat != null && lon != null
        ? ' Last known location: https://maps.google.com/?q=${lat.toStringAsFixed(5)},${lon.toStringAsFixed(5)}'
        : '';
    if (test) return 'Test from AmoledWatch: you are the emergency contact for fall alerts.$where';
    if (sos) return 'AmoledWatch SOS: they asked for help from their watch at $hh:$mm. Please call them now.$where';
    return 'AmoledWatch: a fall was detected at $hh:$mm and not answered on the watch. '
        'Please check on them.$where';
  }
}
