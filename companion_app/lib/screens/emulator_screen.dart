import 'dart:ui' as ui;
import 'package:flutter/material.dart';
import 'package:flutter/scheduler.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:path_provider/path_provider.dart';
import '../emulator/watch_emulator.dart';
import '../providers/ble_provider.dart';
import '../services/ble_service.dart';
import '../theme/app_theme.dart';

/// A watch on the phone: the firmware's own screens and apps, running here.
/// While it's open and no real watch is connected, the app treats it as the
/// watch (BleService.attachVirtual): notifications, music, weather, the
/// assistant's answers, smart replies... reach it like over Bluetooth.
class EmulatorScreen extends ConsumerStatefulWidget {
  const EmulatorScreen({super.key});

  @override
  ConsumerState<EmulatorScreen> createState() => _EmulatorScreenState();
}

class _EmulatorScreenState extends ConsumerState<EmulatorScreen> with SingleTickerProviderStateMixin {
  final WatchEmulator? _emu = WatchEmulator.instance;
  Ticker? _ticker;
  ui.Image? _image;
  int _shownSeq = -1;
  bool _decoding = false;
  double _scale = 1;
  BleService? _ble;
  bool _linked = false;

  @override
  void initState() {
    super.initState();
    final emu = _emu;
    if (emu == null) return;
    getApplicationSupportDirectory().then((d) {
      if (!mounted) return;
      emu.start('${d.path}/watch_emulator');
      _ble = ref.read(bleServiceProvider);
      _linked = _ble!.attachVirtual(emu);
      emu.setConnected(_linked);
      _ticker = createTicker((_) => _tick())..start();
      setState(() {});
    });
  }

  @override
  void dispose() {
    final emu = _emu;
    if (emu != null && _linked) {
      emu.setConnected(false);
      _ble?.detachVirtual(emu);
    }
    _ticker?.dispose();
    _image?.dispose();
    super.dispose();
  }

  void _tick() {
    final emu = _emu!;
    emu.step();
    if (_linked) {
      for (final m in emu.takeMessages()) {
        _ble?.virtualIncoming(m);
      }
    }
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

  void _touch(Offset p, bool down) {
    final emu = _emu!;
    emu.touch((p.dx / _scale).round(), (p.dy / _scale).round(), down);
  }

  Widget _button(String label, int which) {
    final emu = _emu!;
    return GestureDetector(
      onTap: () => emu.button(which, WatchEmulator.shortPress),
      onDoubleTap: () => emu.button(which, WatchEmulator.doublePress),
      onLongPress: () => emu.button(which, WatchEmulator.longPress),
      child: Container(
        padding: const EdgeInsets.symmetric(horizontal: 18, vertical: 10),
        decoration: BoxDecoration(color: AppColors.surfaceLight, borderRadius: BorderRadius.circular(20)),
        child: Text(label, style: const TextStyle(color: AppColors.text, fontWeight: FontWeight.w600)),
      ),
    );
  }

  @override
  Widget build(BuildContext context) {
    final emu = _emu;
    return Scaffold(
      appBar: AppBar(title: const Text('Watch emulator')),
      body: emu == null
          ? const Center(
              child: Padding(
                padding: EdgeInsets.all(32),
                child: Text('The emulator runs on Android only for now.',
                    textAlign: TextAlign.center, style: TextStyle(color: AppColors.textDim)),
              ),
            )
          : LayoutBuilder(builder: (context, c) {
              final size = (c.maxWidth - 32).clamp(200.0, 466.0);
              _scale = size / emu.width;
              return ListView(padding: const EdgeInsets.all(16), children: [
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
                Row(mainAxisAlignment: MainAxisAlignment.center, children: [
                  _button('BOOT', WatchEmulator.boot),
                  const SizedBox(width: 14),
                  _button('PWR', WatchEmulator.power),
                ]),
                const SizedBox(height: 18),
                Text(
                  _linked
                      ? 'Linked to this app like a real watch: notifications, music, weather, the assistant\'s '
                          'answers and smart replies arrive here.'
                      : 'A real watch is connected, so the emulator runs on its own (no phone link).',
                  textAlign: TextAlign.center,
                  style: TextStyle(color: _linked ? AppColors.success : AppColors.warning, fontSize: 12),
                ),
                const SizedBox(height: 8),
                const Text(
                  'The watch\'s own firmware code (screens, apps, games) running on this phone. '
                  'Swipe and tap the screen like the watch. BOOT: tap = back, double = assistant, hold = home. '
                  'PWR: tap = screen off/on. Its settings are kept between runs. Sensors, sound and the '
                  'microphone aren\'t connected yet.',
                  textAlign: TextAlign.center,
                  style: TextStyle(color: AppColors.textDim, fontSize: 12, height: 1.45),
                ),
              ]);
            }),
    );
  }
}
