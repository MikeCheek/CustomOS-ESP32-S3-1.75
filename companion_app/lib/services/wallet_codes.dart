import 'dart:convert';
import 'dart:typed_data';

import 'package:barcode/barcode.dart';
// The module-level classes aren't exported; the watch needs the modules, not a drawing.
// ignore: implementation_imports
import 'package:barcode/src/barcode_1d.dart';
// ignore: implementation_imports
import 'package:barcode/src/barcode_2d.dart';

/// The kinds of code a pass can carry, as the watch names them
/// (firmware/app_wallet.h).
enum CodeKind {
  qr('qr', 'QR code'),
  aztec('aztec', 'Aztec (boarding passes, rail)'),
  pdf417('pdf417', 'PDF417 (boarding passes, IDs)'),
  dataMatrix('datamatrix', 'Data Matrix'),
  code128('code128', 'Code 128 (most loyalty cards)'),
  ean13('ean13', 'EAN-13'),
  ean8('ean8', 'EAN-8'),
  upcA('upca', 'UPC-A'),
  code39('code39', 'Code 39');

  final String key, label;
  const CodeKind(this.key, this.label);

  static CodeKind? fromKey(String k) => CodeKind.values.where((c) => c.key == k).firstOrNull;

  Barcode get barcode => switch (this) {
    CodeKind.qr => Barcode.qrCode(),
    CodeKind.aztec => Barcode.aztec(),
    CodeKind.pdf417 => Barcode.pdf417(),
    CodeKind.dataMatrix => Barcode.dataMatrix(),
    CodeKind.code128 => Barcode.code128(),
    CodeKind.ean13 => Barcode.ean13(),
    CodeKind.ean8 => Barcode.ean8(),
    CodeKind.upcA => Barcode.upcA(),
    CodeKind.code39 => Barcode.code39(),
  };

  bool get linear => barcode is Barcode1D;
}

/// A code turned into its modules: [w] x [h] dark/light squares, row by row
/// (a 1D barcode has h = 1); a module is [ratio] times as tall as it's wide.
class CodeMatrix {
  final int w, h;
  final double ratio;
  final List<bool> dark;
  const CodeMatrix(this.w, this.h, this.ratio, this.dark);

  /// 1 bit a module, MSB first, as the watch takes it.
  Uint8List get bits {
    final out = Uint8List((w * h + 7) >> 3);
    for (var i = 0; i < dark.length; i++) {
      if (dark[i]) out[i >> 3] |= 0x80 >> (i & 7);
    }
    return out;
  }
}

/// The modules for [data] as [kind], or null when [data] can't be that kind
/// (an EAN-13 needs 12 or 13 digits...).
CodeMatrix? encodeCode(CodeKind kind, String data) {
  if (data.isEmpty) return null;
  final b = kind.barcode;
  try {
    if (b is Barcode1D) {
      // ignore: invalid_use_of_protected_member
      final bits = b.convert(data).toList();
      return bits.isEmpty ? null : CodeMatrix(bits.length, 1, 1, bits);
    }
    if (b is Barcode2D) {
      final m = b.convert(Uint8List.fromList(utf8.encode(data)));
      final px = m.pixels.toList();
      final ratio = m.ratio;
      if (px.length != m.width * m.height) return null;
      return CodeMatrix(m.width, m.height, ratio, px);
    }
  } catch (_) {
    return null;
  }
  return null;
}

/// A pass: a name, a line under it, and its code.
class WalletPass {
  final String name, sub, data;
  final CodeKind kind;
  const WalletPass({required this.name, this.sub = '', required this.kind, required this.data});

  Map<String, dynamic> toJson() => {'n': name, 's': sub, 'k': kind.key, 'd': data};
  static WalletPass? fromJson(Map<String, dynamic> j) {
    final k = CodeKind.fromKey(j['k'] as String? ?? '');
    final d = j['d'] as String? ?? '';
    if (k == null || d.isEmpty) return null;
    return WalletPass(name: j['n'] as String? ?? 'Pass', sub: j['s'] as String? ?? '', kind: k, data: d);
  }

  /// What the watch knows it by: changes when the name or the code does.
  int get crc {
    var h = 0x811c9dc5;
    for (final c in utf8.encode('$name|$sub|${kind.key}|$data')) {
      h = ((h ^ c) * 0x01000193) & 0xFFFFFFFF;
    }
    return h & 0x7FFFFFFF;
  }
}

/// The link messages that put [p] in watch slot [slot] (the modules in
/// parts small enough for one message each). Empty if it can't be encoded.
List<Map<String, dynamic>> walletPutMessages(int slot, WalletPass p, {int partBytes = 300}) {
  final m = encodeCode(p.kind, p.data);
  if (m == null) return const [];
  final bits = m.bits;
  if (bits.length > 4096) return const []; // more than the watch keeps
  String cut(String s, int n) => s.length <= n ? s : s.substring(0, n);
  return [
    for (var o = 0; o < bits.length; o += partBytes)
      {
        't': 'wal',
        'op': 'put',
        'i': slot,
        if (o == 0) ...{
          'n': cut(p.name, 30),
          's': cut(p.sub, 46),
          'k': p.kind.key,
          'w': m.w,
          'h': m.h,
          'r': (m.ratio * 10).round().clamp(1, 255),
          'crc': p.crc,
        },
        'tot': bits.length,
        'o': o,
        'b': base64Encode(bits.sublist(o, o + partBytes > bits.length ? bits.length : o + partBytes)),
      },
  ];
}

/// Maps a scanner's format name (ML Kit) to ours.
CodeKind? kindFromScan(String format) => switch (format) {
  'qrCode' => CodeKind.qr,
  'aztec' => CodeKind.aztec,
  'pdf417' => CodeKind.pdf417,
  'dataMatrix' => CodeKind.dataMatrix,
  'code128' => CodeKind.code128,
  'ean13' => CodeKind.ean13,
  'ean8' => CodeKind.ean8,
  'upca' => CodeKind.upcA,
  'code39' => CodeKind.code39,
  _ => null,
};
