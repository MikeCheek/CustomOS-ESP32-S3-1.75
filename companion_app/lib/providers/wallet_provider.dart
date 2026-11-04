import 'dart:convert';

import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:shared_preferences/shared_preferences.dart';

import '../services/wallet_codes.dart';
import 'ble_provider.dart';

/// How many passes the watch keeps (firmware/app_wallet.h WALLET_MAX).
const walletMax = 8;

class WalletState {
  final List<WalletPass> passes;

  /// The watch's slots as it last reported them: the crc, or null when empty.
  final List<int?> onWatch;
  final bool syncing;
  const WalletState({this.passes = const [], this.onWatch = const [], this.syncing = false});

  bool get inSync {
    for (var i = 0; i < walletMax; i++) {
      final want = i < passes.length ? passes[i].crc : null;
      final have = i < onWatch.length ? onWatch[i] : null;
      if (want != have) return false;
    }
    return true;
  }

  WalletState copyWith({List<WalletPass>? passes, List<int?>? onWatch, bool? syncing}) =>
      WalletState(passes: passes ?? this.passes, onWatch: onWatch ?? this.onWatch, syncing: syncing ?? this.syncing);
}

/// The passes, kept on the phone; the watch gets the same, slot by slot
/// ({"t":"wal"}), whenever it reports something different ({"e":"wal"},
/// on every connect and after each change).
class WalletNotifier extends StateNotifier<WalletState> {
  static const _k = 'wallet_passes';
  final Ref _ref;
  WalletNotifier(this._ref) : super(const WalletState()) {
    _load();
  }

  Future<void> _load() async {
    final p = await SharedPreferences.getInstance();
    final list = <WalletPass>[];
    for (final s in p.getStringList(_k) ?? const <String>[]) {
      try {
        final w = WalletPass.fromJson(jsonDecode(s) as Map<String, dynamic>);
        if (w != null) list.add(w);
      } catch (_) {}
    }
    if (mounted) state = state.copyWith(passes: list);
  }

  Future<void> _save() async {
    final p = await SharedPreferences.getInstance();
    await p.setStringList(_k, [for (final w in state.passes) jsonEncode(w.toJson())]);
  }

  /// The watch's list: [["name", crc], [], ...] (an empty one is an empty slot).
  Future<void> onWatch(Map<String, dynamic> e) async {
    final l = e['l'];
    if (l is! List) return;
    state = state.copyWith(
      onWatch: [
        for (final x in l)
          if (x is List && x.length >= 2) (x[1] as num).toInt() else null,
      ],
    );
    await sync();
  }

  bool _busy = false;
  int _tries = 0; // syncs in a row that didn't take (an older watch, no memory): stop at 3

  /// Sends what the watch is missing and clears what it shouldn't have.
  Future<void> sync() async {
    if (_busy) return;
    final ble = _ref.read(bleServiceProvider);
    if (state.inSync) {
      _tries = 0;
      return;
    }
    if (!ble.hasPhoneLink || _tries >= 3) return;
    _tries++;
    _busy = true;
    state = state.copyWith(syncing: true);
    try {
      for (var i = 0; i < walletMax; i++) {
        final want = i < state.passes.length ? state.passes[i] : null;
        final have = i < state.onWatch.length ? state.onWatch[i] : null;
        if (want == null && have != null) {
          await ble.sendLink({'t': 'wal', 'op': 'del', 'i': i});
        } else if (want != null && want.crc != have) {
          for (final m in walletPutMessages(i, want)) {
            await ble.sendLink(m);
          }
        }
      }
    } finally {
      _busy = false;
      if (mounted) state = state.copyWith(syncing: false);
    }
    // the watch answers each change with its list; ask once more to be sure
    await ble.sendLink({'t': 'wal', 'op': 'ask'});
  }

  Future<String?> add(WalletPass p) async {
    _tries = 0;
    if (state.passes.length >= walletMax) return 'The watch keeps $walletMax passes - delete one first';
    if (encodeCode(p.kind, p.data) == null) return 'That can\'t be a ${p.kind.label}';
    state = state.copyWith(passes: [...state.passes, p]);
    await _save();
    await sync();
    return null;
  }

  Future<void> replace(int i, WalletPass p) async {
    _tries = 0;
    final l = [...state.passes];
    if (i < 0 || i >= l.length) return;
    l[i] = p;
    state = state.copyWith(passes: l);
    await _save();
    await sync();
  }

  Future<void> remove(int i) async {
    _tries = 0;
    final l = [...state.passes]..removeAt(i);
    state = state.copyWith(passes: l);
    await _save();
    await sync();
  }

  Future<void> move(int from, int to) async {
    _tries = 0;
    final l = [...state.passes];
    l.insert(to, l.removeAt(from));
    state = state.copyWith(passes: l);
    await _save();
    await sync();
  }
}

final walletProvider = StateNotifierProvider<WalletNotifier, WalletState>((ref) => WalletNotifier(ref));
