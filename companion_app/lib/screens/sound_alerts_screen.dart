import 'dart:async';
import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:permission_handler/permission_handler.dart';
import 'package:shared_preferences/shared_preferences.dart';
import '../providers/ble_provider.dart';
import '../services/sound_alerts.dart';
import '../theme/app_theme.dart';
import '../widgets/common.dart';

/// Sound alerts: the phone listens (while this screen is open; the screen
/// stays on) and the watch shows what it heard.
class SoundAlertsScreen extends ConsumerStatefulWidget {
  const SoundAlertsScreen({super.key});

  @override
  ConsumerState<SoundAlertsScreen> createState() => _SoundAlertsScreenState();
}

class _SoundAlertsScreenState extends ConsumerState<SoundAlertsScreen> {
  final _sa = SoundAlerts.instance;
  bool _installed = false, _busy = false;
  double _progress = 0;
  Set<String> _wanted = {for (final s in SoundAlerts.sounds) s.id};
  final List<(DateTime, AlertSound)> _log = [];
  StreamSubscription<AlertSound>? _sub;

  @override
  void initState() {
    super.initState();
    _sa.isInstalled().then((v) {
      if (mounted) setState(() => _installed = v);
    });
    SharedPreferences.getInstance().then((p) {
      final l = p.getStringList('sound_alerts');
      if (l != null && mounted) setState(() => _wanted = l.toSet());
    });
    _sub = _sa.heard.listen(_onHeard);
  }

  @override
  void dispose() {
    _sub?.cancel();
    _sa.stop();
    super.dispose();
  }

  Future<void> _onHeard(AlertSound s) async {
    final now = DateTime.now();
    if (mounted) setState(() => _log.insert(0, (now, s)));
    final ble = ref.read(bleServiceProvider);
    if (!ble.hasPhoneLink) return;
    await ble.sendLink({
      't': 'ntf',
      'id': 0x534E0000 | (now.millisecondsSinceEpoch & 0xFFFF),
      'ap': 'Sound alert',
      'ti': s.label,
      'tx': 'Heard at ${now.hour.toString().padLeft(2, '0')}:${now.minute.toString().padLeft(2, '0')}',
      'rp': 0,
    });
  }

  Future<void> _install() async {
    setState(() {
      _busy = true;
      _progress = 0;
    });
    try {
      await _sa.install(onProgress: (p) {
        if (mounted) setState(() => _progress = p);
      });
      _installed = true;
    } catch (e) {
      if (mounted) showSnack(context, e.toString().replaceFirst('Exception: ', ''));
    } finally {
      if (mounted) setState(() => _busy = false);
    }
  }

  Future<void> _toggle() async {
    if (_sa.listening) {
      await _sa.stop();
      setState(() {});
      return;
    }
    if (!(await Permission.microphone.request()).isGranted) {
      if (mounted) showSnack(context, 'Microphone access is needed to listen');
      return;
    }
    setState(() {});
    unawaited(_sa.start(_wanted).then((_) {
      if (mounted) setState(() {});
    }));
  }

  @override
  Widget build(BuildContext context) {
    final on = _sa.listening;
    String t(DateTime d) => '${d.hour.toString().padLeft(2, '0')}:${d.minute.toString().padLeft(2, '0')}';
    return Scaffold(
      appBar: AppBar(title: const Text('Sound alerts')),
      body: ListView(padding: const EdgeInsets.fromLTRB(16, 8, 16, 32), children: [
        const Text(
          'The phone listens and your watch shows what it heard - a doorbell, a smoke alarm, a baby crying. '
          'It works while this screen is open (the screen stays on): leave the phone where the sound is. '
          'Sounds are recognised on the phone (sherpa-onnx, CED); nothing is recorded or sent anywhere.',
          style: TextStyle(color: AppColors.textDim, fontSize: 13, height: 1.4),
        ),
        const SizedBox(height: 14),
        if (!_installed)
          Panel(
            child: Row(children: [
              const Expanded(child: Text('Sound model (28 MB)', style: TextStyle(color: AppColors.text))),
              if (_busy)
                SizedBox(width: 80, child: LinearProgressIndicator(value: _progress > 0 ? _progress : null))
              else
                TextButton(onPressed: _install, child: const Text('Download')),
            ]),
          )
        else
          FilledButton.icon(
            onPressed: _toggle,
            icon: Icon(on ? Icons.stop_rounded : Icons.hearing_rounded),
            label: Text(on ? 'Stop listening' : 'Start listening'),
          ),
        const SectionLabel('Tell me about'),
        for (final s in SoundAlerts.sounds)
          CheckboxListTile(
            contentPadding: EdgeInsets.zero,
            title: Text(s.label, style: const TextStyle(color: AppColors.text)),
            value: _wanted.contains(s.id),
            onChanged: on
                ? null
                : (v) async {
                    setState(() => v == true ? _wanted.add(s.id) : _wanted.remove(s.id));
                    await (await SharedPreferences.getInstance()).setStringList('sound_alerts', _wanted.toList());
                  },
          ),
        if (_log.isNotEmpty) ...[
          const SectionLabel('Heard'),
          for (final (d, s) in _log.take(20))
            ListTile(
              contentPadding: EdgeInsets.zero,
              leading: const Icon(Icons.notifications_active_outlined, color: AppColors.warning),
              title: Text(s.label, style: const TextStyle(color: AppColors.text)),
              trailing: Text(t(d), style: const TextStyle(color: AppColors.textDim)),
            ),
        ],
      ]),
    );
  }
}
