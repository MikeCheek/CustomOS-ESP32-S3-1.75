import 'dart:async';
import 'dart:io';
import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:permission_handler/permission_handler.dart';
import '../providers/ble_provider.dart';
import '../providers/notes_provider.dart';
import '../services/phone_recorder.dart';
import '../theme/app_theme.dart';
import '../widgets/common.dart';

/// Records a voice memo with the phone's microphone. It starts as soon as
/// the sheet opens; Save makes it a memo here (transcribed like the watch's)
/// and copies it to the watch's recordings - now, or when it next connects.
Future<void> showPhoneRecordSheet(BuildContext context) async {
  final msg = await showModalBottomSheet<String>(
    context: context,
    isScrollControlled: true,
    isDismissible: false,
    enableDrag: false,
    backgroundColor: AppColors.surface,
    shape: const RoundedRectangleBorder(borderRadius: BorderRadius.vertical(top: Radius.circular(28))),
    builder: (_) => const _PhoneRecordSheet(),
  );
  if (msg != null && context.mounted) showSnack(context, msg);
}

class _PhoneRecordSheet extends ConsumerStatefulWidget {
  const _PhoneRecordSheet();

  @override
  ConsumerState<_PhoneRecordSheet> createState() => _PhoneRecordSheetState();
}

enum _Rec { starting, recording, paused, saving, failed }

class _PhoneRecordSheetState extends ConsumerState<_PhoneRecordSheet> {
  _Rec _st = _Rec.starting;
  String? _error;
  String _name = '';
  int _ms = 0;
  final List<double> _levels = [];
  Timer? _poll;

  @override
  void initState() {
    super.initState();
    _start();
  }

  @override
  void dispose() {
    _poll?.cancel();
    if (_st == _Rec.recording || _st == _Rec.paused) PhoneRecorder.cancel();
    super.dispose();
  }

  Future<void> _start() async {
    final perm = await Permission.microphone.request();
    if (!perm.isGranted) {
      _fail(perm.isPermanentlyDenied
          ? 'Microphone access is off for AmoledWatch - allow it in the phone settings'
          : 'Microphone access is needed to record');
      return;
    }
    _name = NotesNotifier.phoneMemoName();
    final path = await NotesNotifier.phoneMemoPath(_name);
    if (!await PhoneRecorder.start(path)) {
      final p = await PhoneRecorder.poll();
      _fail(p.error ?? 'Could not start the microphone');
      return;
    }
    if (!mounted) {
      PhoneRecorder.cancel();
      return;
    }
    setState(() => _st = _Rec.recording);
    _poll = Timer.periodic(const Duration(milliseconds: 80), (_) async {
      final p = await PhoneRecorder.poll();
      if (!mounted) return;
      if (p.error != null) {
        _fail(p.error!);
        return;
      }
      setState(() {
        _ms = p.ms;
        if (_st == _Rec.recording) {
          _levels.add(p.level / 100.0);
          if (_levels.length > 60) _levels.removeAt(0);
        }
      });
    });
  }

  void _fail(String e) {
    _poll?.cancel();
    if (mounted) setState(() { _st = _Rec.failed; _error = e; });
  }

  Future<void> _togglePause() async {
    if (_st == _Rec.recording) {
      await PhoneRecorder.pause();
      setState(() => _st = _Rec.paused);
    } else if (_st == _Rec.paused) {
      await PhoneRecorder.resume();
      setState(() => _st = _Rec.recording);
    }
  }

  Future<void> _save() async {
    _poll?.cancel();
    setState(() => _st = _Rec.saving);
    try {
      final ms = await PhoneRecorder.stop();
      if (ms < 700) {
        final f = File(await NotesNotifier.phoneMemoPath(_name));
        if (await f.exists()) await f.delete();
        if (mounted) Navigator.pop(context, 'Too short - nothing saved');
        return;
      }
      final msg = await ref.read(notesProvider.notifier).addPhoneRecording(_name, ms);
      if (mounted) Navigator.pop(context, msg);
    } catch (e) {
      _fail(e.toString().replaceFirst('Exception: ', ''));
    }
  }

  Future<void> _cancel() async {
    _poll?.cancel();
    if (_st == _Rec.recording || _st == _Rec.paused) await PhoneRecorder.cancel();
    _st = _Rec.failed;   // nothing left to cancel in dispose()
    if (mounted) Navigator.pop(context);
  }

  @override
  Widget build(BuildContext context) {
    final connected = ref.watch(bleProvider).isConnected;
    final rec = _st == _Rec.recording;
    final secs = _ms ~/ 1000;
    final time = '${secs ~/ 60}:${(secs % 60).toString().padLeft(2, '0')}';
    final title = switch (_st) {
      _Rec.starting => 'Starting…',
      _Rec.recording => 'Recording',
      _Rec.paused => 'Paused',
      _Rec.saving => 'Saving…',
      _Rec.failed => 'Can\'t record',
    };

    return PopScope(
      canPop: false,
      onPopInvokedWithResult: (didPop, _) {
        if (!didPop) _cancel();
      },
      child: SafeArea(
        child: Padding(
          padding: const EdgeInsets.fromLTRB(24, 14, 24, 20),
          child: Column(mainAxisSize: MainAxisSize.min, children: [
            Container(width: 40, height: 4, decoration: BoxDecoration(color: AppColors.border, borderRadius: BorderRadius.circular(2))),
            const SizedBox(height: 18),
            Row(mainAxisAlignment: MainAxisAlignment.center, children: [
              if (rec) ...[
                Container(width: 10, height: 10, decoration: const BoxDecoration(color: AppColors.danger, shape: BoxShape.circle)),
                const SizedBox(width: 8),
              ],
              Text(title, style: const TextStyle(color: AppColors.textDim, fontSize: 14, fontWeight: FontWeight.w600)),
            ]),
            const SizedBox(height: 6),
            Text(time,
                style: const TextStyle(color: AppColors.text, fontSize: 52, fontWeight: FontWeight.w300,
                    fontFeatures: [FontFeature.tabularFigures()])),
            const SizedBox(height: 14),
            SizedBox(
              height: 64,
              width: double.infinity,
              child: CustomPaint(painter: _LevelPainter(_levels, rec ? AppColors.accent3 : AppColors.textFaint)),
            ),
            const SizedBox(height: 22),
            if (_st == _Rec.failed)
              Text(_error ?? '', textAlign: TextAlign.center, style: const TextStyle(color: AppColors.danger, fontSize: 14))
            else
              Row(mainAxisAlignment: MainAxisAlignment.spaceEvenly, children: [
                _RoundButton(icon: Icons.close_rounded, label: 'Discard', onTap: _st == _Rec.saving ? null : _cancel),
                _RoundButton(
                  icon: Icons.stop_rounded,
                  label: 'Save',
                  big: true,
                  color: AppColors.danger,
                  onTap: rec || _st == _Rec.paused ? _save : null,
                ),
                _RoundButton(
                  icon: _st == _Rec.paused ? Icons.mic_rounded : Icons.pause_rounded,
                  label: _st == _Rec.paused ? 'Resume' : 'Pause',
                  onTap: rec || _st == _Rec.paused ? _togglePause : null,
                ),
              ]),
            const SizedBox(height: 18),
            Text(
              connected
                  ? 'Saved as a memo here and copied to the watch'
                  : 'Saved as a memo here - copied to the watch when it connects',
              textAlign: TextAlign.center,
              style: const TextStyle(color: AppColors.textDim, fontSize: 12),
            ),
            if (_st == _Rec.failed) ...[
              const SizedBox(height: 14),
              TextButton(onPressed: () => Navigator.pop(context), child: const Text('Close')),
            ],
          ]),
        ),
      ),
    );
  }
}

class _RoundButton extends StatelessWidget {
  final IconData icon;
  final String label;
  final VoidCallback? onTap;
  final bool big;
  final Color color;
  const _RoundButton({required this.icon, required this.label, this.onTap, this.big = false, this.color = AppColors.surfaceHigh});

  @override
  Widget build(BuildContext context) {
    final size = big ? 78.0 : 56.0;
    return Opacity(
      opacity: onTap == null ? 0.4 : 1,
      child: Column(mainAxisSize: MainAxisSize.min, children: [
        Material(
          color: color,
          shape: const CircleBorder(),
          child: InkWell(
            customBorder: const CircleBorder(),
            onTap: onTap,
            child: SizedBox(width: size, height: size, child: Icon(icon, color: AppColors.text, size: big ? 38 : 26)),
          ),
        ),
        const SizedBox(height: 6),
        Text(label, style: const TextStyle(color: AppColors.textDim, fontSize: 12)),
      ]),
    );
  }
}

/// Recent input levels as centred bars, newest on the right.
class _LevelPainter extends CustomPainter {
  final List<double> levels;
  final Color color;
  _LevelPainter(this.levels, this.color);

  @override
  void paint(Canvas canvas, Size size) {
    const n = 60;
    final w = size.width / n;
    final p = Paint()
      ..color = color
      ..strokeCap = StrokeCap.round
      ..strokeWidth = w * 0.55;
    final mid = size.height / 2;
    for (var i = 0; i < n; i++) {
      final li = levels.length - n + i;
      final v = li >= 0 ? levels[li] : 0.0;
      final h = 3 + (size.height - 6) * (v < 0 ? 0 : (v > 1 ? 1 : v));
      final x = w * (i + 0.5);
      canvas.drawLine(Offset(x, mid - h / 2), Offset(x, mid + h / 2), p);
    }
  }

  @override
  bool shouldRepaint(covariant _LevelPainter old) => true;
}
