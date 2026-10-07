import 'dart:math';
import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';
import '../providers/ble_provider.dart';
import '../services/ble_service.dart';
import '../theme/app_theme.dart';

/// Virtual game controller: a draggable joystick plus 4 action buttons
/// (A/B/X/Y), sent to the watch over BLE on every state change. The
/// watch mirrors whatever this last sent, so it drives movement in
/// whichever game is open, or pans/selects in the watch's own app
/// menu if no game is running - see hal_controller.h on the firmware
/// side, which doesn't distinguish where the input goes, only that
/// something is currently reading it.
///
/// The joystick is a UI improvement over the old 4 separate d-pad
/// buttons (much easier to drag smoothly than tapping discrete
/// buttons one at a time) but the actual BLE packet is still the same
/// digital up/down/left/right bitmask the firmware already expects -
/// the joystick's drag position gets thresholded into those same 4
/// bits here (supporting diagonals, e.g. up+right together), rather
/// than changing the wire protocol and needing to re-verify all 11
/// games' controller integration for an analog value they never asked
/// for.
class ControllerScreen extends ConsumerStatefulWidget {
  const ControllerScreen({super.key});

  @override
  ConsumerState<ControllerScreen> createState() => _ControllerScreenState();
}

class _ControllerScreenState extends ConsumerState<ControllerScreen> {
  static const double _baseRadius = 90;
  static const double _knobRadius = 34;
  static const double _deadzone = 0.35; // fraction of _baseRadius before any direction registers

  Offset _knobOffset = Offset.zero; // relative to the base's center, clamped to _baseRadius
  bool _up = false, _down = false, _left = false, _right = false;
  bool _a = false, _b = false, _x = false, _y = false;

  BleService? _svc;

  @override
  void dispose() {
    // Leaving the screen mid-press would otherwise leave a direction or
    // button held down on the watch until the next connection.
    _svc?.sendController().catchError((_) {});
    super.dispose();
  }

  void _send() {
    // Remembered so dispose() can send "all released" without touching ref.
    final svc = ref.read(bleServiceProvider);
    _svc = svc;
    svc.sendController(
          up: _up, down: _down, left: _left, right: _right,
          a: _a, b: _b, x: _x, y: _y,
        ).catchError((_) {});
  }

  void _updateJoystick(Offset localPosition, Size baseSize) {
    final center = Offset(baseSize.width / 2, baseSize.height / 2);
    Offset delta = localPosition - center;
    final dist = delta.distance;
    if (dist > _baseRadius) {
      delta = Offset(delta.dx / dist * _baseRadius, delta.dy / dist * _baseRadius);
    }

    final norm = dist > 0 ? min(dist, _baseRadius) / _baseRadius : 0.0;
    bool up = false, down = false, left = false, right = false;
    if (norm >= _deadzone) {
      final angle = atan2(delta.dy, delta.dx); // 0 = right, +pi/2 = down (screen coords)
      // 8-way sector check so diagonals set two bits together, same
      // as pressing two d-pad buttons at once used to.
      if (angle > -3 * pi / 8 && angle < 3 * pi / 8) right = true;
      if (angle > pi / 8 && angle < 7 * pi / 8) down = true;
      if (angle > 5 * pi / 8 || angle < -5 * pi / 8) left = true;
      if (angle > -7 * pi / 8 && angle < -pi / 8) up = true;
    }

    setState(() => _knobOffset = delta);

    if (up != _up || down != _down || left != _left || right != _right) {
      _up = up; _down = down; _left = left; _right = right;
      _send();
    }
  }

  void _releaseJoystick() {
    setState(() => _knobOffset = Offset.zero);
    if (_up || _down || _left || _right) {
      _up = _down = _left = _right = false;
      _send();
    }
  }

  void _setButton(String which, bool pressed) {
    setState(() {
      switch (which) {
        case 'a': _a = pressed; break;
        case 'b': _b = pressed; break;
        case 'x': _x = pressed; break;
        case 'y': _y = pressed; break;
      }
    });
    _send();
  }

  Widget _joystick() {
    const size = Size(_baseRadius * 2, _baseRadius * 2);
    return GestureDetector(
      onPanStart: (d) => _updateJoystick(d.localPosition, size),
      onPanUpdate: (d) => _updateJoystick(d.localPosition, size),
      onPanEnd: (_) => _releaseJoystick(),
      onPanCancel: _releaseJoystick,
      child: Container(
        width: size.width,
        height: size.height,
        decoration: BoxDecoration(
          color: AppColors.surface,
          shape: BoxShape.circle,
          border: Border.all(color: AppColors.textDim.withOpacity(0.4), width: 2),
        ),
        child: Center(
          child: Transform.translate(
            offset: _knobOffset,
            child: Container(
              width: _knobRadius * 2,
              height: _knobRadius * 2,
              decoration: BoxDecoration(
                color: (_up || _down || _left || _right) ? AppColors.accent3 : AppColors.accent,
                shape: BoxShape.circle,
              ),
            ),
          ),
        ),
      ),
    );
  }

  Widget _actionButton(String label, Color color, String which) {
    return GestureDetector(
      onTapDown: (_) => _setButton(which, true),
      onTapUp: (_) => _setButton(which, false),
      onTapCancel: () => _setButton(which, false),
      child: Container(
        width: 72,
        height: 72,
        decoration: BoxDecoration(color: color, shape: BoxShape.circle),
        alignment: Alignment.center,
        child: Text(
          label,
          style: const TextStyle(color: Colors.white, fontWeight: FontWeight.bold, fontSize: 22),
        ),
      ),
    );
  }

  Widget _buttonCluster() {
    return SizedBox(
      width: 200,
      height: 200,
      child: Stack(
        children: [
          Positioned(top: 0, left: 64, child: _actionButton('Y', AppColors.warning, 'y')),
          Positioned(bottom: 0, left: 64, child: _actionButton('A', AppColors.accent, 'a')),
          Positioned(left: 0, top: 64, child: _actionButton('X', AppColors.accent2, 'x')),
          Positioned(right: 0, top: 64, child: _actionButton('B', AppColors.danger, 'b')),
        ],
      ),
    );
  }

  @override
  Widget build(BuildContext context) {
    final connected = ref.watch(bleProvider).isConnected;

    return Scaffold(
      appBar: AppBar(title: const Text('Controller')),
      body: Column(
        children: [
          if (!connected)
            Container(
              margin: const EdgeInsets.all(16),
              padding: const EdgeInsets.all(12),
              width: double.infinity,
              decoration: BoxDecoration(
                color: AppColors.danger.withOpacity(0.15),
                borderRadius: BorderRadius.circular(10),
              ),
              child: const Text(
                'Not connected to watch',
                style: TextStyle(color: AppColors.danger),
                textAlign: TextAlign.center,
              ),
            ),
          Expanded(
            child: Row(
              mainAxisAlignment: MainAxisAlignment.spaceEvenly,
              children: [
                _joystick(),
                _buttonCluster(),
              ],
            ),
          ),
          const Padding(
            padding: EdgeInsets.only(bottom: 24),
            child: Text(
              'Drag to move, A selects/attacks in the\nwatch\'s current game or app menu',
              textAlign: TextAlign.center,
              style: TextStyle(color: AppColors.textDim, fontSize: 12),
            ),
          ),
        ],
      ),
    );
  }
}
