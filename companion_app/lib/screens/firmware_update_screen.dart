import 'dart:io';
import 'package:file_picker/file_picker.dart';
import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';
import '../providers/ble_provider.dart';
import '../providers/companion_provider.dart';
import '../services/firmware_image.dart';
import '../theme/app_theme.dart';
import '../widgets/common.dart';

/// Pick a firmware .bin and install it on the watch over Bluetooth.
class FirmwareUpdateScreen extends ConsumerStatefulWidget {
  const FirmwareUpdateScreen({super.key});

  @override
  ConsumerState<FirmwareUpdateScreen> createState() => _FirmwareUpdateScreenState();
}

enum _Phase { pick, ready, sending, done, failed }

class _FirmwareUpdateScreenState extends ConsumerState<FirmwareUpdateScreen> {
  FirmwareImage? _image;
  _Phase _phase = _Phase.pick;
  int _written = 0;
  String? _error;
  bool _cancel = false;
  DateTime? _startedAt;

  Future<void> _pick() async {
    try {
      final r = await FilePicker.platform.pickFiles(type: FileType.any, withData: false);
      final f = r?.files.single;
      if (f == null || f.path == null) return;
      final bytes = await File(f.path!).readAsBytes();
      final img = FirmwareImage.parse(f.name, bytes);
      setState(() {
        _image = img;
        _phase = _Phase.ready;
        _error = null;
      });
    } on FormatException catch (e) {
      setState(() {
        _image = null;
        _phase = _Phase.pick;
        _error = e.message;
      });
    } catch (e) {
      setState(() => _error = 'Could not read the file: $e');
    }
  }

  Future<void> _install() async {
    final img = _image;
    if (img == null) return;
    final ble = ref.read(bleServiceProvider);
    setState(() {
      _phase = _Phase.sending;
      _written = 0;
      _cancel = false;
      _error = null;
      _startedAt = DateTime.now();
    });
    try {
      await ble.sendFirmware(
        img.bytes,
        signature: img.signature,
        onProgress: (w, _) {
          if (mounted) setState(() => _written = w);
        },
        cancel: () => _cancel || !mounted,
      );
      if (mounted) setState(() => _phase = _Phase.done);
    } catch (e) {
      if (mounted) {
        setState(() {
          _phase = _Phase.failed;
          _error = e.toString().replaceFirst('Exception: ', '');
        });
      }
    }
  }

  String _eta() {
    final img = _image, start = _startedAt;
    if (img == null || start == null || _written < 32 * 1024) return '';
    final secs = DateTime.now().difference(start).inMilliseconds / 1000.0;
    final rate = _written / secs; // bytes/s
    final left = ((img.bytes.length - _written) / rate).round();
    final kbs = (rate / 1024).toStringAsFixed(0);
    return '$kbs KB/s · ${left ~/ 60}:${(left % 60).toString().padLeft(2, '0')} left';
  }

  @override
  Widget build(BuildContext context) {
    final ble = ref.watch(bleProvider);
    final link = ref.watch(companionProvider);
    final svc = ref.read(bleServiceProvider);
    final img = _image;
    final current = link.watchFirmware;
    final busy = _phase == _Phase.sending;

    return PopScope(
      canPop: !busy,
      onPopInvokedWithResult: (didPop, _) {
        if (!didPop && busy) showSnack(context, 'Cancel the update first');
      },
      child: Scaffold(
        appBar: AppBar(title: const Text('Firmware update')),
        body: ListView(
          padding: const EdgeInsets.fromLTRB(16, 8, 16, 32),
          children: [
            Panel(
              child: Row(children: [
                const IconChip(Icons.watch_rounded, color: AppColors.accent),
                const SizedBox(width: 14),
                Expanded(
                  child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
                    Text(ble.deviceName ?? 'Watch',
                        style: const TextStyle(color: AppColors.text, fontSize: 16, fontWeight: FontWeight.w600)),
                    const SizedBox(height: 2),
                    Text(
                      !ble.isConnected
                          ? 'Not connected'
                          : 'Firmware ${current ?? 'older than 2.5'}${ble.batteryLevel >= 0 ? ' · battery ${ble.batteryLevel}%' : ''}',
                      style: const TextStyle(color: AppColors.textDim, fontSize: 13),
                    ),
                  ]),
                ),
              ]),
            ),
            const SizedBox(height: 12),
            if (ble.isConnected && !svc.supportsOta)
              const _Note(
                icon: Icons.usb_rounded,
                color: AppColors.warning,
                text: 'This watch is running firmware without Bluetooth updates. Flash version 2.5 or later '
                    'once over USB from the Arduino IDE - after that, updates can be installed from here.',
              ),
            if (_phase == _Phase.pick || _phase == _Phase.ready) ...[
              Panel(
                onTap: busy ? null : _pick,
                child: Row(children: [
                  const IconChip(Icons.file_open_rounded, color: AppColors.accent2),
                  const SizedBox(width: 14),
                  Expanded(
                    child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
                      Text(img == null ? 'Choose firmware file' : img.name,
                          style: const TextStyle(color: AppColors.text, fontSize: 15, fontWeight: FontWeight.w600)),
                      const SizedBox(height: 2),
                      Text(
                        img == null
                            ? 'AmoledSmartWatchOS.ino.bin from the Arduino build folder'
                            : 'Version ${img.version} · ${(img.bytes.length / 1024).round()} KB${img.isSigned ? ' · signed' : ''}',
                        style: const TextStyle(color: AppColors.textDim, fontSize: 13),
                      ),
                    ]),
                  ),
                  const Icon(Icons.chevron_right_rounded, color: AppColors.textFaint),
                ]),
              ),
              if (img != null && current != null) ...[
                const SizedBox(height: 10),
                _versionNote(img.version!, current),
              ],
            ],
            if (_error != null && _phase != _Phase.failed) ...[
              const SizedBox(height: 10),
              _Note(icon: Icons.error_outline_rounded, color: AppColors.danger, text: _error!),
            ],
            if (_phase == _Phase.sending || _phase == _Phase.done || _phase == _Phase.failed) ...[
              const SizedBox(height: 8),
              _progressPanel(img),
            ],
            const SizedBox(height: 20),
            if (_phase == _Phase.ready)
              FilledButton.icon(
                icon: const Icon(Icons.system_update_rounded),
                label: const Text('Install on watch'),
                onPressed: ble.isConnected && svc.supportsOta ? _install : null,
              ),
            if (_phase == _Phase.sending)
              OutlinedButton(
                onPressed: () => setState(() => _cancel = true),
                child: const Text('Cancel'),
              ),
            if (_phase == _Phase.failed)
              FilledButton(
                onPressed: () => setState(() => _phase = img == null ? _Phase.pick : _Phase.ready),
                child: const Text('Try again'),
              ),
            if (_phase == _Phase.done)
              FilledButton(onPressed: () => Navigator.pop(context), child: const Text('Done')),
            const SizedBox(height: 16),
            const Text(
              'The watch keeps its current firmware until the new one is fully received and verified. '
              'If the new firmware fails to start, the watch goes back to the previous one by itself.',
              style: TextStyle(color: AppColors.textDim, fontSize: 12, height: 1.45),
            ),
          ],
        ),
      ),
    );
  }

  Widget _versionNote(String next, String current) {
    final c = FirmwareImage.compareVersions(next, current);
    if (c > 0) {
      return _Note(icon: Icons.upgrade_rounded, color: AppColors.success, text: 'Update from $current to $next');
    }
    return _Note(
      icon: Icons.info_outline_rounded,
      color: AppColors.warning,
      text: c == 0 ? 'The watch already runs $next - installing it again is fine.' : 'This is older than the watch\'s $current.',
    );
  }

  Widget _progressPanel(FirmwareImage? img) {
    final total = img?.bytes.length ?? 1;
    final frac = _phase == _Phase.done ? 1.0 : (_written / total).clamp(0.0, 1.0);
    final color = _phase == _Phase.failed
        ? AppColors.danger
        : _phase == _Phase.done
            ? AppColors.success
            : AppColors.accent;
    return Panel(
      child: Column(children: [
        Ring(
          value: frac,
          size: 150,
          stroke: 12,
          color: color,
          child: Column(mainAxisSize: MainAxisSize.min, children: [
            Text('${(frac * 100).round()}%',
                style: const TextStyle(color: AppColors.text, fontSize: 30, fontWeight: FontWeight.w700)),
          ]),
        ),
        const SizedBox(height: 16),
        Text(
          switch (_phase) {
            _Phase.sending => _written == 0 ? 'Starting…' : 'Installing - keep the watch close',
            _Phase.done => 'Installed. The watch is restarting.',
            _Phase.failed => _error ?? 'Update failed',
            _ => '',
          },
          textAlign: TextAlign.center,
          style: TextStyle(color: _phase == _Phase.failed ? AppColors.danger : AppColors.text, fontSize: 15),
        ),
        if (_phase == _Phase.sending) ...[
          const SizedBox(height: 4),
          Text(_eta(), style: const TextStyle(color: AppColors.textDim, fontSize: 12)),
        ],
      ]),
    );
  }
}

class _Note extends StatelessWidget {
  final IconData icon;
  final Color color;
  final String text;
  const _Note({required this.icon, required this.color, required this.text});

  @override
  Widget build(BuildContext context) {
    return Container(
      padding: const EdgeInsets.all(14),
      decoration: BoxDecoration(
        color: color.withValues(alpha: 0.10),
        borderRadius: BorderRadius.circular(16),
        border: Border.all(color: color.withValues(alpha: 0.35)),
      ),
      child: Row(crossAxisAlignment: CrossAxisAlignment.start, children: [
        Icon(icon, color: color, size: 20),
        const SizedBox(width: 10),
        Expanded(child: Text(text, style: const TextStyle(color: AppColors.text, fontSize: 13, height: 1.4))),
      ]),
    );
  }
}
