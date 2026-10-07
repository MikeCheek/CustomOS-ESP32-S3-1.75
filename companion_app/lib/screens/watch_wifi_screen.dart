import 'dart:async';
import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';
import '../providers/ble_provider.dart';
import '../services/ble_service.dart';
import '../theme/app_theme.dart';
import '../widgets/common.dart';

/// Set up and check the watch's Wi-Fi from the phone (firmware 2.9+): see
/// what the watch is connected to, scan from the watch, send a network and
/// password, and test the connection. Protocol: link message "wcfg".
class WatchWifiScreen extends ConsumerStatefulWidget {
  const WatchWifiScreen({super.key});

  @override
  ConsumerState<WatchWifiScreen> createState() => _WatchWifiScreenState();
}

class _Net {
  final String ssid;
  final int rssi;
  final bool open;
  const _Net(this.ssid, this.rssi, this.open);
}

class _WatchWifiScreenState extends ConsumerState<WatchWifiScreen> {
  StreamSubscription? _sub;
  Map<String, dynamic>? _status;
  List<_Net>? _nets;
  bool _scanning = false;
  bool _testing = false;
  String? _testingSsid;
  String? _result;       // last test outcome, shown in the status card
  bool _resultOk = false;
  Timer? _timeout;

  BleService get _ble => ref.read(bleServiceProvider);

  @override
  void initState() {
    super.initState();
    _sub = _ble.linkEvents.listen(_onEvent);
    WidgetsBinding.instance.addPostFrameCallback((_) => _send({'op': 'status'}));
  }

  @override
  void dispose() {
    _sub?.cancel();
    _timeout?.cancel();
    super.dispose();
  }

  Future<void> _send(Map<String, dynamic> m, {Duration? wait}) async {
    if (!_ble.hasPhoneLink) return;
    try {
      await _ble.sendLink({'t': 'wcfg', ...m});
    } catch (e) {
      if (mounted) showSnack(context, 'Couldn\'t reach the watch');
      return;
    }
    if (wait != null) {
      _timeout?.cancel();
      _timeout = Timer(wait, () {
        if (!mounted) return;
        setState(() {
          if (_scanning) _scanning = false;
          if (_testing) {
            _testing = false;
            _result = 'The watch didn\'t answer';
            _resultOk = false;
          }
        });
      });
    }
  }

  void _onEvent(Map<String, dynamic> e) {
    if (e['e'] != 'wcfg' || !mounted) return;
    final op = e['op'];
    setState(() {
      if (op == 'scan') {
        _timeout?.cancel();
        _scanning = false;
        if (e['ok'] == 1) {
          _nets = [
            for (final n in (e['l'] as List? ?? const []))
              _Net('${(n as Map)['s']}', (n['r'] as num?)?.toInt() ?? -100, n['o'] == 1)
          ];
        } else {
          showSnack(context, '${e['why'] ?? 'Scan failed'}');
        }
      } else if (op == 'test') {
        _timeout?.cancel();
        _testing = false;
        _resultOk = e['ok'] == 1;
        _result = _resultOk ? 'Connected' : '${e['why'] ?? 'Couldn\'t connect'}';
        if (e.containsKey('saved')) _status = e;
      } else {
        _status = e;
        if (e['ok'] == 0 && e['why'] != null) showSnack(context, '${e['why']}');
      }
    });
  }

  void _scan() {
    setState(() {
      _scanning = true;
      _nets = null;
    });
    _send({'op': 'scan'}, wait: const Duration(seconds: 20));
  }

  void _test() {
    setState(() {
      _testing = true;
      _testingSsid = _status?['saved'] as String?;
      _result = null;
    });
    _send({'op': 'test'}, wait: const Duration(seconds: 25));
  }

  Future<void> _join(String ssid, {required bool open}) async {
    String? pass = '';
    if (!open) {
      pass = await _askPassword(ssid);
      if (pass == null) return;
    }
    setState(() {
      _testing = true;
      _testingSsid = ssid;
      _result = null;
    });
    _send({'op': 'set', 's': ssid, 'p': pass}, wait: const Duration(seconds: 25));
  }

  Future<String?> _askPassword(String ssid) {
    final ctl = TextEditingController();
    var obscure = true;
    return showDialog<String>(
      context: context,
      builder: (ctx) => StatefulBuilder(
        builder: (ctx, set) => AlertDialog(
          title: Text(ssid),
          content: TextField(
            controller: ctl,
            autofocus: true,
            obscureText: obscure,
            decoration: InputDecoration(
              hintText: 'Password',
              suffixIcon: IconButton(
                icon: Icon(obscure ? Icons.visibility_rounded : Icons.visibility_off_rounded),
                onPressed: () => set(() => obscure = !obscure),
              ),
            ),
            onSubmitted: (v) => Navigator.pop(ctx, v),
          ),
          actions: [
            TextButton(onPressed: () => Navigator.pop(ctx), child: const Text('Cancel')),
            TextButton(onPressed: () => Navigator.pop(ctx, ctl.text), child: const Text('Connect')),
          ],
        ),
      ),
    );
  }

  Future<void> _manual() async {
    final ctl = TextEditingController();
    final ssid = await showDialog<String>(
      context: context,
      builder: (ctx) => AlertDialog(
        title: const Text('Network name'),
        content: TextField(
          controller: ctl,
          autofocus: true,
          decoration: const InputDecoration(hintText: 'SSID (2.4 GHz)'),
          onSubmitted: (v) => Navigator.pop(ctx, v.trim()),
        ),
        actions: [
          TextButton(onPressed: () => Navigator.pop(ctx), child: const Text('Cancel')),
          TextButton(onPressed: () => Navigator.pop(ctx, ctl.text.trim()), child: const Text('Next')),
        ],
      ),
    );
    if (ssid != null && ssid.isNotEmpty) await _join(ssid, open: false);
  }

  void _forget() {
    showDialog(
      context: context,
      builder: (ctx) => AlertDialog(
        title: const Text('Forget network?'),
        content: Text('The watch forgets "${_status?['saved']}" and turns Wi-Fi off.'),
        actions: [
          TextButton(onPressed: () => Navigator.pop(ctx), child: const Text('Cancel')),
          TextButton(
            onPressed: () {
              Navigator.pop(ctx);
              setState(() => _result = null);
              _send({'op': 'forget'});
            },
            child: const Text('Forget', style: TextStyle(color: AppColors.danger)),
          ),
        ],
      ),
    );
  }

  static int _bars(int rssi) => rssi >= -55 ? 4 : rssi >= -67 ? 3 : rssi >= -75 ? 2 : 1;

  static IconData _signalIcon(int rssi) => switch (_bars(rssi)) {
        4 => Icons.network_wifi_rounded,
        3 => Icons.network_wifi_3_bar_rounded,
        2 => Icons.network_wifi_2_bar_rounded,
        _ => Icons.network_wifi_1_bar_rounded,
      };

  @override
  Widget build(BuildContext context) {
    final ble = ref.watch(bleProvider);
    final svc = ref.read(bleServiceProvider);
    if (!ble.isConnected) {
      return Scaffold(
        appBar: AppBar(title: const Text('Watch Wi-Fi')),
        body: const EmptyState(
            icon: Icons.watch_off_outlined, title: 'Watch not connected', subtitle: 'Connect the watch to set up its Wi-Fi.'),
      );
    }
    if (!svc.wifiSetup) {
      return Scaffold(
        appBar: AppBar(title: const Text('Watch Wi-Fi')),
        body: const EmptyState(
            icon: Icons.system_update_rounded,
            title: 'Update the watch',
            subtitle: 'Setting up Wi-Fi from the app needs watch firmware 2.9 or newer.'),
      );
    }
    final st = _status;
    final saved = (st?['saved'] as String?) ?? '';
    final connected = st?['conn'] == 1;
    final rssi = (st?['rssi'] as num?)?.toInt();
    return Scaffold(
      appBar: AppBar(
        title: const Text('Watch Wi-Fi'),
        actions: [
          IconButton(tooltip: 'Refresh', onPressed: () => _send({'op': 'status'}), icon: const Icon(Icons.refresh_rounded)),
        ],
      ),
      body: ListView(
        padding: const EdgeInsets.fromLTRB(16, 8, 16, 32),
        children: [
          Panel(
            gradient: LinearGradient(
              begin: Alignment.topLeft,
              end: Alignment.bottomRight,
              colors: [(connected ? AppColors.success : AppColors.accent).withValues(alpha: 0.18), AppColors.surface],
            ),
            child: st == null
                ? const Row(children: [
                    SizedBox(width: 20, height: 20, child: CircularProgressIndicator(strokeWidth: 2)),
                    SizedBox(width: 14),
                    Text('Asking the watch...', style: TextStyle(color: AppColors.textDim)),
                  ])
                : Column(
                    crossAxisAlignment: CrossAxisAlignment.start,
                    children: [
                      Row(children: [
                        IconChip(
                          connected ? _signalIcon(rssi ?? -80) : (saved.isEmpty ? Icons.wifi_off_rounded : Icons.wifi_rounded),
                          color: connected ? AppColors.success : AppColors.accent,
                        ),
                        const SizedBox(width: 14),
                        Expanded(
                          child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
                            Text(saved.isEmpty ? 'No network saved' : saved,
                                style: const TextStyle(color: AppColors.text, fontSize: 17, fontWeight: FontWeight.w700)),
                            const SizedBox(height: 2),
                            Text(
                              connected
                                  ? 'Connected · ${st['ip']} · ${rssi ?? '?'} dBm'
                                  : st['on'] == 1
                                      ? 'Wi-Fi on, not connected'
                                      : st['auto'] == 1
                                          ? 'Wi-Fi off'
                                          : 'Wi-Fi off (turns on for syncs and transfers)',
                              style: const TextStyle(color: AppColors.textDim, fontSize: 13),
                            ),
                          ]),
                        ),
                      ]),
                      if (st['err'] != null) ...[
                        const SizedBox(height: 10),
                        Text('${st['err']}', style: const TextStyle(color: AppColors.danger, fontSize: 13)),
                      ],
                      if (_testing || _result != null) ...[
                        const SizedBox(height: 14),
                        Row(children: [
                          if (_testing)
                            const SizedBox(width: 18, height: 18, child: CircularProgressIndicator(strokeWidth: 2))
                          else
                            Icon(_resultOk ? Icons.check_circle_rounded : Icons.error_outline_rounded,
                                size: 20, color: _resultOk ? AppColors.success : AppColors.danger),
                          const SizedBox(width: 10),
                          Expanded(
                            child: Text(
                              _testing ? 'Connecting to ${_testingSsid ?? 'the network'}...' : _result!,
                              style: TextStyle(
                                  color: _testing ? AppColors.text : (_resultOk ? AppColors.success : AppColors.danger),
                                  fontSize: 14,
                                  fontWeight: FontWeight.w600),
                            ),
                          ),
                        ]),
                      ],
                      const SizedBox(height: 14),
                      Row(children: [
                        if (saved.isNotEmpty)
                          Expanded(
                            child: FilledButton.icon(
                              onPressed: _testing ? null : _test,
                              icon: const Icon(Icons.network_check_rounded, size: 18),
                              label: const Text('Test connection'),
                            ),
                          ),
                        if (saved.isNotEmpty) const SizedBox(width: 10),
                        if (saved.isNotEmpty)
                          OutlinedButton(onPressed: _testing ? null : _forget, child: const Text('Forget')),
                      ]),
                    ],
                  ),
          ),
          SectionLabel('Networks the watch sees',
              trailing: TextButton.icon(
                onPressed: _scanning || _testing ? null : _scan,
                icon: _scanning
                    ? const SizedBox(width: 14, height: 14, child: CircularProgressIndicator(strokeWidth: 2))
                    : const Icon(Icons.wifi_find_rounded, size: 18),
                label: Text(_scanning ? 'Scanning' : 'Scan'),
              )),
          if (_nets == null && !_scanning)
            Panel(
              onTap: _scan,
              child: const Row(children: [
                Icon(Icons.wifi_find_rounded, color: AppColors.textDim),
                SizedBox(width: 12),
                Expanded(
                  child: Text('Scan from the watch to pick a network. The watch only sees 2.4 GHz networks.',
                      style: TextStyle(color: AppColors.textDim, fontSize: 13)),
                ),
              ]),
            )
          else if (_nets != null && _nets!.isEmpty)
            const Panel(child: Text('No networks found', style: TextStyle(color: AppColors.textDim)))
          else if (_nets != null)
            RowGroup(children: [
              for (final n in _nets!)
                NavRow(
                  icon: _signalIcon(n.rssi),
                  color: n.ssid == saved && connected ? AppColors.success : AppColors.accent2,
                  title: n.ssid,
                  subtitle: [
                    if (n.ssid == saved) connected ? 'Connected' : 'Saved',
                    n.open ? 'Open' : 'Secured',
                    '${n.rssi} dBm',
                  ].join(' · '),
                  trailing: Icon(n.open ? Icons.lock_open_rounded : Icons.lock_rounded, size: 18, color: AppColors.textFaint),
                  onTap: _testing ? null : () => _join(n.ssid, open: n.open),
                ),
            ]),
          const SizedBox(height: 8),
          TextButton.icon(
            onPressed: _testing ? null : _manual,
            icon: const Icon(Icons.edit_rounded, size: 18),
            label: const Text('Enter a hidden network'),
          ),
          const SizedBox(height: 8),
          const Text(
            'The watch uses Wi-Fi for time sync and fast file transfers. The password is sent to the watch '
            'over Bluetooth and saved only there.',
            style: TextStyle(color: AppColors.textFaint, fontSize: 12, height: 1.4),
          ),
        ],
      ),
    );
  }
}
