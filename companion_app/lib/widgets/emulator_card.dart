import 'dart:io';
import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';

import '../emulator/emulator_controller.dart';
import '../providers/ble_provider.dart';
import '../screens/emulator_screen.dart';
import '../theme/app_theme.dart';
import 'common.dart';

/// The watch emulator's on/off switch, its state, "Keep it as my watch" and
/// a way to open its screen - the same card on Home, Apps and Settings.
class EmulatorCard extends ConsumerStatefulWidget {
  /// Without the "keep" switch and the explanation (Home).
  final bool compact;
  const EmulatorCard({super.key, this.compact = false});

  @override
  ConsumerState<EmulatorCard> createState() => _EmulatorCardState();
}

class _EmulatorCardState extends ConsumerState<EmulatorCard> {
  late final EmulatorController _ctl = ref.read(emulatorProvider);
  StreamSubscription<void>? _sub;
  bool _keep = false, _busy = false;

  @override
  void initState() {
    super.initState();
    _sub = _ctl.changes.listen((_) => _reload());
    _reload();
  }

  Future<void> _reload() async {
    final k = await EmulatorController.keepRunning();
    if (mounted) setState(() => _keep = k);
  }

  @override
  void dispose() {
    _sub?.cancel();
    super.dispose();
  }

  Future<void> _toggle(bool on) async {
    setState(() => _busy = true);
    try {
      if (on) {
        await _ctl.start();
      } else {
        await _ctl.setKeepRunning(false); // off means off, also at the next start
        await _ctl.stop();
      }
    } finally {
      if (mounted) setState(() => _busy = false);
    }
  }

  void _open() => Navigator.push(context, MaterialPageRoute(builder: (_) => const EmulatorScreen()));

  Future<void> _float() async {
    final want = !_ctl.floating;
    final ok = await _ctl.float(want);
    if (want && !ok && mounted) {
      ScaffoldMessenger.of(context).showSnackBar(
        const SnackBar(content: Text('Allow "Display over other apps" for this app, then turn it on again')),
      );
    }
    if (mounted) setState(() {});
  }

  @override
  Widget build(BuildContext context) {
    ref.watch(bleProvider); // linked or not follows the connection
    final available = _ctl.available;
    final on = _ctl.running;
    final linked = on && _ctl.linked;

    final String status;
    final Color color;
    if (!available) {
      status = 'Not available on this phone';
      color = AppColors.textFaint;
    } else if (!on) {
      status = 'Off';
      color = AppColors.textDim;
    } else if (linked) {
      status = 'On · it\'s your watch now';
      color = AppColors.success;
    } else {
      status = 'On · a real watch is connected';
      color = AppColors.warning;
    }

    return Panel(
      padding: const EdgeInsets.fromLTRB(16, 14, 10, 14),
      color: on ? AppColors.accent3.withValues(alpha: 0.08) : null,
      onTap: available && on ? _open : null,
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [
          Row(
            children: [
              const IconChip(Icons.developer_board_rounded, color: AppColors.accent3),
              const SizedBox(width: 14),
              Expanded(
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    const Text(
                      'Watch emulator',
                      style: TextStyle(color: AppColors.text, fontSize: 15, fontWeight: FontWeight.w600),
                    ),
                    const SizedBox(height: 6),
                    StatusPill(status, color: color),
                  ],
                ),
              ),
              if (_busy)
                const Padding(
                  padding: EdgeInsets.all(12),
                  child: SizedBox(width: 22, height: 22, child: CircularProgressIndicator(strokeWidth: 2)),
                )
              else
                Switch(value: on, onChanged: available ? _toggle : null),
            ],
          ),
          if (!widget.compact) ...[
            const SizedBox(height: 10),
            const Text(
              'The watch\'s own firmware running on this phone, linked to the app like a real watch whenever '
              'none is connected: notifications, music, the assistant, faces and games, without the hardware.',
              style: TextStyle(color: AppColors.textDim, fontSize: 12, height: 1.4),
            ),
          ],
          if (on && available) ...[
            const SizedBox(height: 8),
            Row(
              children: [
                if (!widget.compact)
                  Expanded(
                    child: InkWell(
                      borderRadius: BorderRadius.circular(12),
                      onTap: () => _ctl.setKeepRunning(!_keep),
                      child: Row(
                        children: [
                          Checkbox(value: _keep, onChanged: (v) => _ctl.setKeepRunning(v ?? false)),
                          const Flexible(
                            child: Text(
                              'Keep it on after the app closes',
                              style: TextStyle(color: AppColors.text, fontSize: 13),
                            ),
                          ),
                        ],
                      ),
                    ),
                  )
                else
                  const Spacer(),
                FilledButton.tonalIcon(
                  onPressed: _open,
                  icon: const Icon(Icons.open_in_full_rounded, size: 18),
                  label: const Text('Open'),
                ),
              ],
            ),
            if (Platform.isAndroid)
              InkWell(
                borderRadius: BorderRadius.circular(12),
                onTap: _float,
                child: Row(
                  children: [
                    Switch(value: _ctl.floating, onChanged: (_) => _float()),
                    const SizedBox(width: 6),
                    const Expanded(
                      child: Text(
                        'Float over other apps - drag the rim to move it, pinch to resize, push it past a side '
                        'to tuck it away, drop it on ✕ to close',
                        style: TextStyle(color: AppColors.text, fontSize: 12.5, height: 1.3),
                      ),
                    ),
                  ],
                ),
              ),
          ],
        ],
      ),
    );
  }
}
