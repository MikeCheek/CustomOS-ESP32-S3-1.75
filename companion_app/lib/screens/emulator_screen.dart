import 'dart:async';
import 'dart:ui' as ui;
import 'package:flutter/material.dart';
import 'package:flutter/scheduler.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';
import '../emulator/emulator_controller.dart';
import '../emulator/watch_emulator.dart';
import '../theme/app_theme.dart';

/// A watch on the phone: the firmware's own screens and apps, running here
/// (emulator/emulator_controller.dart runs it; this screen shows it and
/// passes touches and buttons). Linked to the app as the watch whenever no
/// real watch is connected; "Keep it as my watch" keeps it running and
/// linked after this screen closes, and with the next app start.
class EmulatorScreen extends ConsumerStatefulWidget {
  const EmulatorScreen({super.key});

  @override
  ConsumerState<EmulatorScreen> createState() => _EmulatorScreenState();
}

class _EmulatorScreenState extends ConsumerState<EmulatorScreen> with SingleTickerProviderStateMixin {
  late final EmulatorController _ctl = ref.read(emulatorProvider);
  WatchEmulator? get _emu => _ctl.emu;
  Ticker? _ticker;
  ui.Image? _image;
  int _shownSeq = -1;
  bool _decoding = false, _keep = false;
  bool _startedHere = false; // turned on by opening this screen: off again when it closes
  double _scale = 1;
  StreamSubscription<void>? _changes;

  @override
  void initState() {
    super.initState();
    if (_emu == null) return;
    _changes = _ctl.changes.listen((_) {
      if (mounted) setState(() {});
    });
    EmulatorController.keepRunning().then((k) {
      if (mounted) setState(() => _keep = k);
    });
    _startedHere = !_ctl.running;
    _ctl.start().then((_) {
      if (!mounted) return;
      _ctl.visible = true;
      _ticker = createTicker((_) => _render())..start();
      setState(() {});
    });
  }

  @override
  void dispose() {
    _changes?.cancel();
    _ctl.visible = false;
    if (_startedHere && !_keep) _ctl.stop();
    _ticker?.dispose();
    _image?.dispose();
    super.dispose();
  }

  // The controller steps the watch; this only shows its new frames.
  void _render() {
    final emu = _emu!;
    final seq = emu.frameSeq;
    if (seq == _shownSeq || _decoding) return;
    _decoding = true;
    ui.decodeImageFromPixels(emu.frame(), emu.width, emu.height, ui.PixelFormat.rgba8888, (img) {
      _decoding = false;
      if (!mounted) {
        img.dispose();
        return;
      }
      setState(() {
        _image?.dispose();
        _image = img;
        _shownSeq = seq;
      });
    });
  }

  void _touch(Offset p, bool down) => _emu!.touch((p.dx / _scale).round(), (p.dy / _scale).round(), down);

  Widget _button(String label, int which) {
    final emu = _emu!;
    return GestureDetector(
      onTap: () => emu.button(which, WatchEmulator.shortPress),
      onDoubleTap: () => emu.button(which, WatchEmulator.doublePress),
      onLongPress: () => emu.button(which, WatchEmulator.longPress),
      child: Container(
        padding: const EdgeInsets.symmetric(horizontal: 18, vertical: 10),
        decoration: BoxDecoration(color: AppColors.surfaceLight, borderRadius: BorderRadius.circular(20)),
        child: Text(
          label,
          style: const TextStyle(color: AppColors.text, fontWeight: FontWeight.w600),
        ),
      ),
    );
  }

  @override
  Widget build(BuildContext context) {
    final emu = _emu;
    final linked = _ctl.linked;
    return Scaffold(
      appBar: AppBar(title: const Text('Watch emulator')),
      body: emu == null
          ? const Center(
              child: Padding(
                padding: EdgeInsets.all(32),
                child: Text(
                  'The emulator runs on Android only for now.',
                  textAlign: TextAlign.center,
                  style: TextStyle(color: AppColors.textDim),
                ),
              ),
            )
          : LayoutBuilder(
              builder: (context, c) {
                final size = (c.maxWidth - 32).clamp(200.0, 466.0);
                _scale = size / emu.width;
                return ListView(
                  padding: const EdgeInsets.all(16),
                  children: [
                    Center(
                      child: Container(
                        padding: const EdgeInsets.all(10),
                        decoration: const BoxDecoration(shape: BoxShape.circle, color: Color(0xFF26262F)),
                        child: ClipOval(
                          child: SizedBox(
                            width: size,
                            height: size,
                            child: Listener(
                              onPointerDown: (e) => _touch(e.localPosition, true),
                              onPointerMove: (e) => _touch(e.localPosition, true),
                              onPointerUp: (e) => _touch(e.localPosition, false),
                              onPointerCancel: (e) => _touch(e.localPosition, false),
                              child: _image == null
                                  ? const ColoredBox(color: Colors.black)
                                  : RawImage(image: _image, width: size, height: size, fit: BoxFit.fill),
                            ),
                          ),
                        ),
                      ),
                    ),
                    const SizedBox(height: 18),
                    Row(
                      mainAxisAlignment: MainAxisAlignment.center,
                      children: [
                        _button('BOOT', WatchEmulator.boot),
                        const SizedBox(width: 14),
                        _button('PWR', WatchEmulator.power),
                      ],
                    ),
                    const SizedBox(height: 12),
                    SwitchListTile(
                      contentPadding: const EdgeInsets.symmetric(horizontal: 4),
                      title: const Text('Keep it as my watch', style: TextStyle(color: AppColors.text, fontSize: 14)),
                      subtitle: const Text(
                        'Keeps running and linked when you leave this screen and when the app restarts, '
                        'until a real watch connects (uses some battery)',
                        style: TextStyle(color: AppColors.textDim, fontSize: 12),
                      ),
                      value: _keep,
                      onChanged: (v) async {
                        await _ctl.setKeepRunning(v);
                        setState(() => _keep = v);
                      },
                    ),
                    const SizedBox(height: 6),
                    Text(
                      linked
                          ? 'Linked to this app as the watch: notifications, calls, music, weather, the assistant, '
                                'smart replies, Catch me up, timers and files arrive here.'
                          : 'A real watch is connected: it stays the watch, the emulator runs on its own.',
                      textAlign: TextAlign.center,
                      style: TextStyle(color: linked ? AppColors.success : AppColors.warning, fontSize: 12),
                    ),
                    const SizedBox(height: 8),
                    const Text(
                      'The watch\'s own firmware code (screens, apps, games) running on this phone. '
                      'Swipe and tap the screen like the watch. BOOT: tap = back, double = assistant, hold = home. '
                      'PWR: tap = screen off/on. Its settings and SD card are kept. Tilting the phone tilts the watch '
                      '(games, fall detection, sleep), its sounds play here and it records with the phone\'s microphone '
                      '(recorder, assistant, voice replies).',
                      textAlign: TextAlign.center,
                      style: TextStyle(color: AppColors.textDim, fontSize: 12, height: 1.45),
                    ),
                  ],
                );
              },
            ),
    );
  }
}
