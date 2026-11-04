import 'dart:convert';
import 'dart:io';

import 'package:amoled_companion/services/wallet_codes.dart';
import 'package:flutter_test/flutter_test.dart';

void main() {
  test('a QR code becomes square modules, sent in parts that put it back together', () {
    const p = WalletPass(name: 'Gym', sub: 'Member 0042', kind: CodeKind.qr, data: 'https://example.com/m/0042');
    final m = encodeCode(p.kind, p.data)!;
    expect(m.w, m.h);
    expect(m.w, greaterThanOrEqualTo(21));
    final msgs = walletPutMessages(2, p, partBytes: 20);
    expect(msgs.length, greaterThan(1));
    expect(msgs.first['w'], m.w);
    final bytes = <int>[];
    for (final x in msgs) {
      expect(x['o'], bytes.length);
      bytes.addAll(base64Decode(x['b'] as String));
      expect(jsonEncode(x).length, lessThan(700)); // one link message each
    }
    expect(bytes, m.bits);
  });

  test('barcodes are one row, PDF417 has tall modules', () {
    final c = encodeCode(CodeKind.code128, 'LOYAL123456')!;
    expect(c.h, 1);
    expect(c.w, greaterThan(50));
    final pdf = encodeCode(CodeKind.pdf417, 'M1DOE/JOHN EABC123 FCOLHRBA 0123 123Y012A0001 100')!;
    expect(pdf.ratio, greaterThan(1));
    expect(encodeCode(CodeKind.ean13, 'not digits'), isNull);
  });

  test('scanner formats map to ours; the crc follows the content', () {
    expect(kindFromScan('aztec'), CodeKind.aztec);
    expect(kindFromScan('unknown'), isNull);
    const a = WalletPass(name: 'A', kind: CodeKind.qr, data: 'x');
    const b = WalletPass(name: 'A', kind: CodeKind.qr, data: 'y');
    expect(a.crc == b.crc, isFalse);
  });

  // The simulator's Wallet shots use these (tools/watch_sim/wallet_sample.json).
  test('writes the simulator sample', () {
    const passes = [
      WalletPass(
        name: 'Ryanair FR 1234',
        sub: 'Dublin - Rome, seat 12A, gate 23',
        kind: CodeKind.aztec,
        data: 'M1DOE/JOHN            EABC123 DUBFCOFR 1234 312Y012A0001 100',
      ),
      WalletPass(name: 'Gym', sub: 'Member 0042', kind: CodeKind.qr, data: 'https://example.com/m/0042'),
      WalletPass(name: 'Supermarket', sub: 'Loyalty card', kind: CodeKind.code128, data: '2400012345678'),
    ];
    final msgs = [for (var i = 0; i < passes.length; i++) ...walletPutMessages(i, passes[i])];
    File('../tools/watch_sim/wallet_sample.json').writeAsStringSync(msgs.map(jsonEncode).join('\n'));
  });
}
