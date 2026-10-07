import 'dart:typed_data';

/// A watch firmware .bin picked by the user, checked before flashing.
class FirmwareImage {
  final String name;
  final Uint8List bytes;
  final String? version; // from the AMOLEDWATCH_FW=<version> marker
  final Uint8List? signature; // from a *.signed.bin (scripts/sign_firmware.py)

  FirmwareImage._(this.name, this.bytes, this.version, [this.signature]);

  bool get isSigned => signature != null;

  static const _sigTrailer = 'AWSIG001';

  /// The watch's app slots are 3 MB (partition scheme app3M_fat9M_16MB).
  static const maxSize = 3 * 1024 * 1024;
  static const _marker = 'AMOLEDWATCH_FW=';

  /// Throws a readable message if [bytes] isn't a firmware for this watch.
  static FirmwareImage parse(String name, Uint8List bytes) {
    // Signed image: image + signature(64) + "AWSIG001".
    Uint8List? sig;
    if (bytes.length > 1024 + 72 &&
        String.fromCharCodes(bytes.sublist(bytes.length - 8)) == _sigTrailer) {
      sig = Uint8List.fromList(bytes.sublist(bytes.length - 72, bytes.length - 8));
      bytes = Uint8List.sublistView(bytes, 0, bytes.length - 72);
    }
    if (bytes.length < 1024 || bytes[0] != 0xE9) {
      throw const FormatException(
          'Not an ESP32 app image. Pick the .ino.bin file from the Arduino build folder (not the merged or bootloader .bin).');
    }
    // An app image has its esp_app_desc_t (magic 0xABCD5432) right after the
    // 24-byte image header and the first 8-byte segment header. Bootloader
    // and merged (bootloader + partitions + app) images don't.
    if (ByteData.sublistView(bytes).getUint32(0x20, Endian.little) != 0xABCD5432) {
      throw const FormatException(
          'This is a bootloader or merged image. Pick firmware.ino.bin (the app only), or a .bin from the GitHub build.');
    }
    if (bytes.length > maxSize) {
      throw const FormatException('The image is bigger than the watch\'s 3 MB app slot.');
    }
    final version = _findVersion(bytes);
    if (version == null) {
      throw const FormatException('This isn\'t AmoledSmartWatchOS firmware (version marker not found).');
    }
    return FirmwareImage._(name, bytes, version, sig);
  }

  static String? _findVersion(Uint8List b) {
    final m = _marker.codeUnits;
    outer:
    for (var i = 0; i + m.length < b.length; i++) {
      if (b[i] != m[0]) continue;
      for (var k = 1; k < m.length; k++) {
        if (b[i + k] != m[k]) continue outer;
      }
      final start = i + m.length;
      var end = start;
      while (end < b.length && end - start < 24 && b[end] >= 0x20 && b[end] < 0x7F) {
        end++;
      }
      if (end > start) return String.fromCharCodes(b.sublist(start, end));
    }
    return null;
  }

  /// -1 if a < b, 0 if equal, 1 if a > b ("2.10.0" > "2.9.1").
  static int compareVersions(String a, String b) {
    final pa = a.split('.').map((s) => int.tryParse(s.replaceAll(RegExp(r'[^0-9]'), '')) ?? 0).toList();
    final pb = b.split('.').map((s) => int.tryParse(s.replaceAll(RegExp(r'[^0-9]'), '')) ?? 0).toList();
    for (var i = 0; i < 3; i++) {
      final x = i < pa.length ? pa[i] : 0, y = i < pb.length ? pb[i] : 0;
      if (x != y) return x < y ? -1 : 1;
    }
    return 0;
  }
}
