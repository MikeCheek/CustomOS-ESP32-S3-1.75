import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:google_mlkit_barcode_scanning/google_mlkit_barcode_scanning.dart' as mlkit;
import 'package:image_picker/image_picker.dart';

import '../providers/ble_provider.dart';
import '../providers/wallet_provider.dart';
import '../services/wallet_codes.dart';
import '../theme/app_theme.dart';
import '../widgets/common.dart';

/// Boarding passes, tickets and cards on the watch: add one from a photo or
/// a screenshot of its code (or type it), and the watch shows it big and
/// bright, ready to scan (Wallet on the watch).
class WalletScreen extends ConsumerWidget {
  const WalletScreen({super.key});

  @override
  Widget build(BuildContext context, WidgetRef ref) {
    final w = ref.watch(walletProvider);
    final connected = ref.watch(bleProvider).isConnected;
    final n = ref.read(walletProvider.notifier);

    return Scaffold(
      appBar: AppBar(title: const Text('Wallet')),
      floatingActionButton: w.passes.length < walletMax
          ? FloatingActionButton.extended(
              onPressed: () => _add(context, ref),
              icon: const Icon(Icons.add_rounded),
              label: const Text('Add a pass'),
            )
          : null,
      body: ListView(
        padding: const EdgeInsets.fromLTRB(16, 4, 16, 96),
        children: [
          Padding(
            padding: const EdgeInsets.only(bottom: 10),
            child: Row(
              children: [
                Icon(
                  !connected
                      ? Icons.watch_off_outlined
                      : w.syncing
                      ? Icons.sync_rounded
                      : w.inSync
                      ? Icons.check_circle_rounded
                      : Icons.schedule_rounded,
                  size: 18,
                  color: connected && w.inSync ? AppColors.success : AppColors.textDim,
                ),
                const SizedBox(width: 8),
                Expanded(
                  child: Text(
                    !connected
                        ? 'The watch gets them when it connects'
                        : w.syncing
                        ? 'Sending to the watch...'
                        : w.inSync
                        ? 'On the watch: Wallet in the apps'
                        : 'Waiting for the watch',
                    style: const TextStyle(color: AppColors.textDim, fontSize: 13),
                  ),
                ),
              ],
            ),
          ),
          if (w.passes.isEmpty)
            const EmptyState(
              icon: Icons.qr_code_2_rounded,
              title: 'No passes yet',
              subtitle:
                  'Add a boarding pass, a ticket or a loyalty card: pick a screenshot or a photo of its code, '
                  'and it shows on the watch, ready to scan.',
            )
          else
            Panel(
              padding: const EdgeInsets.symmetric(vertical: 4),
              child: ReorderableListView(
                shrinkWrap: true,
                physics: const NeverScrollableScrollPhysics(),
                buildDefaultDragHandles: false,
                onReorderItem: (from, to) => n.move(from, to),
                children: [
                  for (var i = 0; i < w.passes.length; i++)
                    ListTile(
                      key: ValueKey('${w.passes[i].crc}-$i'),
                      leading: SizedBox(width: 44, height: 44, child: CodePreview(pass: w.passes[i])),
                      title: Text(w.passes[i].name),
                      subtitle: Text(
                        [
                          if (w.passes[i].sub.isNotEmpty) w.passes[i].sub,
                          w.passes[i].kind.label.split(' (').first,
                        ].join(' · '),
                        style: const TextStyle(color: AppColors.textDim, fontSize: 12.5),
                      ),
                      onTap: () => _edit(context, ref, i, w.passes[i]),
                      trailing: ReorderableDragStartListener(
                        index: i,
                        child: const Padding(
                          padding: EdgeInsets.all(8),
                          child: Icon(Icons.drag_handle_rounded, color: AppColors.textFaint),
                        ),
                      ),
                    ),
                ],
              ),
            ),
          const Padding(
            padding: EdgeInsets.fromLTRB(4, 12, 4, 0),
            child: Text(
              'On the watch the code fills the screen at full brightness and stays on until you tap it. '
              'The first one in this list is the first there. Passes live on the watch\'s SD card.',
              style: TextStyle(color: AppColors.textFaint, fontSize: 12),
            ),
          ),
        ],
      ),
    );
  }

  static Future<void> _add(BuildContext context, WidgetRef ref) async {
    final how = await showModalBottomSheet<String>(
      context: context,
      showDragHandle: true,
      builder: (ctx) => SafeArea(
        child: Column(
          mainAxisSize: MainAxisSize.min,
          children: [
            ListTile(
              leading: const Icon(Icons.photo_library_outlined),
              title: const Text('From a screenshot or photo'),
              subtitle: const Text('A boarding pass, ticket or card with its code'),
              onTap: () => Navigator.pop(ctx, 'gallery'),
            ),
            ListTile(
              leading: const Icon(Icons.photo_camera_outlined),
              title: const Text('Take a photo of the code'),
              onTap: () => Navigator.pop(ctx, 'camera'),
            ),
            ListTile(
              leading: const Icon(Icons.keyboard_outlined),
              title: const Text('Type it in'),
              subtitle: const Text('The number on a loyalty card, a link...'),
              onTap: () => Navigator.pop(ctx, 'type'),
            ),
          ],
        ),
      ),
    );
    if (how == null || !context.mounted) return;
    WalletPass? start;
    if (how != 'type') {
      start = await _scan(context, how == 'camera' ? ImageSource.camera : ImageSource.gallery);
      if (start == null) return;
    }
    if (!context.mounted) return;
    final p = await showDialog<WalletPass>(
      context: context,
      builder: (_) => _PassEditor(start: start),
    );
    if (p == null) return;
    final err = await ref.read(walletProvider.notifier).add(p);
    if (err != null && context.mounted) {
      ScaffoldMessenger.of(context).showSnackBar(SnackBar(content: Text(err)));
    }
  }

  /// The first code found in a picked image.
  static Future<WalletPass?> _scan(BuildContext context, ImageSource src) async {
    final img = await ImagePicker().pickImage(source: src, maxWidth: 2400);
    if (img == null) return null;
    final scanner = mlkit.BarcodeScanner(formats: [mlkit.BarcodeFormat.all]);
    try {
      final codes = await scanner.processImage(mlkit.InputImage.fromFilePath(img.path));
      for (final c in codes) {
        final k = kindFromScan(c.format.name);
        final v = c.rawValue;
        if (k != null && v != null && v.isNotEmpty) {
          return WalletPass(name: _guessName(k, v), kind: k, data: v);
        }
      }
    } catch (_) {
      // no scanner on this device: fall through
    } finally {
      scanner.close();
    }
    if (context.mounted) {
      ScaffoldMessenger.of(
        context,
      ).showSnackBar(const SnackBar(content: Text('No code found there - try a sharper picture, or type it in')));
    }
    return null;
  }

  /// A boarding pass (IATA BCBP: "M1NAME/..." then the flight) names itself.
  static String _guessName(CodeKind k, String v) {
    final bcbp = RegExp(r'^M1.{20}.{8}([A-Z]{3})([A-Z]{3})([A-Z0-9 ]{3})(\d{4,5})').firstMatch(v);
    if (bcbp != null) {
      return '${bcbp.group(3)!.trim()} ${int.tryParse(bcbp.group(4)!) ?? bcbp.group(4)} '
          '${bcbp.group(1)}-${bcbp.group(2)}';
    }
    return switch (k) {
      CodeKind.qr => 'QR code',
      CodeKind.aztec || CodeKind.pdf417 => 'Ticket',
      _ => 'Card',
    };
  }

  static Future<void> _edit(BuildContext context, WidgetRef ref, int i, WalletPass p) async {
    final n = ref.read(walletProvider.notifier);
    final r = await showDialog<Object>(
      context: context,
      builder: (_) => _PassEditor(start: p, canDelete: true),
    );
    if (r is WalletPass) await n.replace(i, r);
    if (r == 'delete') await n.remove(i);
  }
}

/// Name, the line under it, the kind of code and what it says.
class _PassEditor extends StatefulWidget {
  final WalletPass? start;
  final bool canDelete;
  const _PassEditor({this.start, this.canDelete = false});

  @override
  State<_PassEditor> createState() => _PassEditorState();
}

class _PassEditorState extends State<_PassEditor> {
  late final _name = TextEditingController(text: widget.start?.name ?? '');
  late final _sub = TextEditingController(text: widget.start?.sub ?? '');
  late final _data = TextEditingController(text: widget.start?.data ?? '');
  late CodeKind _kind = widget.start?.kind ?? CodeKind.code128;

  WalletPass get _pass => WalletPass(
    name: _name.text.trim().isEmpty ? 'Pass' : _name.text.trim(),
    sub: _sub.text.trim(),
    kind: _kind,
    data: _data.text,
  );

  @override
  Widget build(BuildContext context) {
    final ok = encodeCode(_kind, _data.text) != null;
    return AlertDialog(
      title: Text(widget.canDelete ? 'Pass' : 'New pass'),
      content: SingleChildScrollView(
        child: Column(
          mainAxisSize: MainAxisSize.min,
          children: [
            SizedBox(
              height: 140,
              child: ok
                  ? CodePreview(pass: _pass)
                  : const Center(
                      child: Text('This kind of code can\'t say that', style: TextStyle(color: AppColors.textDim)),
                    ),
            ),
            const SizedBox(height: 12),
            TextField(
              controller: _name,
              decoration: const InputDecoration(labelText: 'Name', hintText: 'Ryanair FR 1234, Gym, Supermarket'),
              onChanged: (_) => setState(() {}),
            ),
            TextField(
              controller: _sub,
              decoration: const InputDecoration(labelText: 'Under it (optional)', hintText: 'Seat 12A, gate 23'),
            ),
            const SizedBox(height: 8),
            DropdownButtonFormField<CodeKind>(
              initialValue: _kind,
              isExpanded: true,
              decoration: const InputDecoration(labelText: 'Kind of code'),
              items: [for (final k in CodeKind.values) DropdownMenuItem(value: k, child: Text(k.label))],
              onChanged: (k) => setState(() => _kind = k ?? _kind),
            ),
            TextField(
              controller: _data,
              minLines: 1,
              maxLines: 4,
              decoration: const InputDecoration(labelText: 'What the code says'),
              onChanged: (_) => setState(() {}),
            ),
          ],
        ),
      ),
      actions: [
        if (widget.canDelete)
          TextButton(
            onPressed: () => Navigator.pop(context, 'delete'),
            child: const Text('Delete', style: TextStyle(color: AppColors.danger)),
          ),
        TextButton(onPressed: () => Navigator.pop(context), child: const Text('Cancel')),
        FilledButton(onPressed: ok ? () => Navigator.pop(context, _pass) : null, child: const Text('Save')),
      ],
    );
  }
}

/// The code as the watch draws it: black modules on white.
class CodePreview extends StatelessWidget {
  final WalletPass pass;
  const CodePreview({super.key, required this.pass});

  @override
  Widget build(BuildContext context) {
    final m = encodeCode(pass.kind, pass.data);
    return Container(
      decoration: BoxDecoration(color: Colors.white, borderRadius: BorderRadius.circular(6)),
      padding: const EdgeInsets.all(4),
      child: m == null ? const SizedBox.shrink() : CustomPaint(painter: _CodePainter(m), size: Size.infinite),
    );
  }
}

class _CodePainter extends CustomPainter {
  final CodeMatrix m;
  _CodePainter(this.m);

  @override
  void paint(Canvas canvas, Size size) {
    final p = Paint()..color = Colors.black;
    if (m.h == 1) {
      final mw = size.width / m.w;
      for (var x = 0; x < m.w; x++) {
        if (m.dark[x]) canvas.drawRect(Rect.fromLTWH(x * mw, 0, mw + 0.2, size.height), p);
      }
      return;
    }
    final s = (size.width / m.w) < (size.height / (m.h * m.ratio)) ? size.width / m.w : size.height / (m.h * m.ratio);
    final ox = (size.width - m.w * s) / 2, oy = (size.height - m.h * m.ratio * s) / 2;
    for (var y = 0; y < m.h; y++) {
      for (var x = 0; x < m.w; x++) {
        if (m.dark[y * m.w + x]) {
          canvas.drawRect(Rect.fromLTWH(ox + x * s, oy + y * m.ratio * s, s + 0.2, m.ratio * s + 0.2), p);
        }
      }
    }
  }

  @override
  bool shouldRepaint(_CodePainter o) => o.m != m;
}
