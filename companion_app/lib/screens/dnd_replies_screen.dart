import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';
import '../providers/ble_provider.dart';
import '../providers/settings_provider.dart';
import '../theme/app_theme.dart';
import '../widgets/common.dart';

/// Do Not Disturb sync + bedtime, and the quick replies offered on the
/// watch (firmware 3.1+).
class DndRepliesScreen extends ConsumerStatefulWidget {
  const DndRepliesScreen({super.key});

  @override
  ConsumerState<DndRepliesScreen> createState() => _DndRepliesScreenState();
}

class _DndRepliesScreenState extends ConsumerState<DndRepliesScreen> with WidgetsBindingObserver {
  static const _defaults = ['OK', 'On my way', 'Call you later', 'Thanks!', 'Yes', 'No', "Can't talk now"];
  bool _access = true;
  bool _phoneDnd = false;

  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addObserver(this);
    _refresh();
  }

  @override
  void dispose() {
    WidgetsBinding.instance.removeObserver(this);
    super.dispose();
  }

  // Back from the system "Do Not Disturb access" page.
  @override
  void didChangeAppLifecycleState(AppLifecycleState s) {
    if (s == AppLifecycleState.resumed) _refresh();
  }

  Future<void> _refresh() async {
    final d = await ref.read(nativeServiceProvider).getDnd();
    if (mounted) setState(() {
      _access = d.access;
      _phoneDnd = d.on;
    });
  }

  String _fmt(int min) => '${(min ~/ 60).toString().padLeft(2, '0')}:${(min % 60).toString().padLeft(2, '0')}';

  Future<int?> _pickTime(int min) async {
    final t = await showTimePicker(
      context: context,
      initialTime: TimeOfDay(hour: min ~/ 60, minute: min % 60),
      builder: (ctx, child) => MediaQuery(data: MediaQuery.of(ctx).copyWith(alwaysUse24HourFormat: true), child: child!),
    );
    return t == null ? null : t.hour * 60 + t.minute;
  }

  Future<String?> _editReply(String initial) {
    final ctl = TextEditingController(text: initial);
    return showDialog<String>(
      context: context,
      builder: (ctx) => AlertDialog(
        title: Text(initial.isEmpty ? 'New reply' : 'Edit reply'),
        content: TextField(
          controller: ctl,
          autofocus: true,
          maxLength: 38,
          decoration: const InputDecoration(hintText: 'e.g. In a meeting, call you later'),
          onSubmitted: (v) => Navigator.pop(ctx, v.trim()),
        ),
        actions: [
          TextButton(onPressed: () => Navigator.pop(ctx), child: const Text('Cancel')),
          TextButton(onPressed: () => Navigator.pop(ctx, ctl.text.trim()), child: const Text('Save')),
        ],
      ),
    );
  }

  @override
  Widget build(BuildContext context) {
    final s = ref.watch(settingsProvider);
    final setter = ref.read(settingsProvider.notifier);
    final replies = s.quickReplies.isEmpty ? _defaults : s.quickReplies;
    void saveReplies(List<String> l) => setter.update(s.copyWith(quickReplies: l));

    return Scaffold(
      appBar: AppBar(title: const Text('Do Not Disturb & replies')),
      body: ListView(
        padding: const EdgeInsets.fromLTRB(16, 8, 16, 32),
        children: [
          const SectionLabel('Do Not Disturb'),
          RowGroup(children: [
            ToggleRow(
              icon: Icons.do_not_disturb_on_outlined,
              title: 'Sync with the phone',
              subtitle: _phoneDnd ? 'Phone is in Do Not Disturb now' : 'Phone and watch switch together',
              value: s.dndSync,
              onChanged: (v) => setter.update(s.copyWith(dndSync: v)),
            ),
            if (s.dndSync && !_access)
              NavRow(
                icon: Icons.lock_open_rounded,
                color: AppColors.warning,
                title: 'Allow Do Not Disturb access',
                subtitle: 'So the watch can switch the phone too (one-time system setting)',
                onTap: () => ref.read(nativeServiceProvider).openDndAccess(),
              ),
            ToggleRow(
              icon: Icons.bedtime_outlined,
              color: AppColors.accent2,
              title: 'Bedtime on the watch',
              subtitle: 'Silent every night, also when the phone is away',
              value: s.bedtimeOn,
              onChanged: (v) => setter.update(s.copyWith(bedtimeOn: v)),
            ),
            if (s.bedtimeOn) ...[
              NavRow(
                icon: Icons.nightlight_round,
                color: AppColors.accent2,
                title: 'From',
                subtitle: _fmt(s.bedFrom),
                onTap: () async {
                  final m = await _pickTime(s.bedFrom);
                  if (m != null) setter.update(ref.read(settingsProvider).copyWith(bedFrom: m));
                },
              ),
              NavRow(
                icon: Icons.wb_sunny_outlined,
                color: AppColors.warning,
                title: 'Until',
                subtitle: _fmt(s.bedTo),
                onTap: () async {
                  final m = await _pickTime(s.bedTo);
                  if (m != null) setter.update(ref.read(settingsProvider).copyWith(bedTo: m));
                },
              ),
            ],
          ]),
          SectionLabel('Quick replies',
              trailing: s.quickReplies.isEmpty
                  ? null
                  : TextButton(onPressed: () => saveReplies(const []), child: const Text('Reset'))),
          const Padding(
            padding: EdgeInsets.fromLTRB(4, 0, 4, 10),
            child: Text('Offered on the watch when you reply to a message, after "Dictate". Up to 10; drag to reorder.',
                style: TextStyle(color: AppColors.textDim, fontSize: 13)),
          ),
          Panel(
            padding: const EdgeInsets.symmetric(vertical: 4),
            child: ReorderableListView(
              shrinkWrap: true,
              physics: const NeverScrollableScrollPhysics(),
              buildDefaultDragHandles: false,
              onReorder: (a, b) {
                final l = [...replies];
                if (b > a) b--;
                l.insert(b, l.removeAt(a));
                saveReplies(l);
              },
              children: [
                for (var i = 0; i < replies.length; i++)
                  ListTile(
                    key: ValueKey('qr$i${replies[i]}'),
                    leading: ReorderableDragStartListener(
                        index: i, child: const Icon(Icons.drag_indicator_rounded, color: AppColors.textFaint)),
                    title: Text(replies[i]),
                    onTap: () async {
                      final v = await _editReply(replies[i]);
                      if (v == null) return;
                      final l = [...replies];
                      if (v.isEmpty) {
                        l.removeAt(i);
                      } else {
                        l[i] = v;
                      }
                      saveReplies(l);
                    },
                    trailing: IconButton(
                      icon: const Icon(Icons.close_rounded, size: 20),
                      onPressed: replies.length <= 1 ? null : () => saveReplies([...replies]..removeAt(i)),
                    ),
                  ),
              ],
            ),
          ),
          const SizedBox(height: 8),
          if (replies.length < 10)
            OutlinedButton.icon(
              onPressed: () async {
                final v = await _editReply('');
                if (v != null && v.isNotEmpty) saveReplies([...replies, v]);
              },
              icon: const Icon(Icons.add_rounded),
              label: const Text('Add a reply'),
            ),
          const SizedBox(height: 8),
          const Text('Needs watch firmware 3.1 or newer; changes are sent when the watch is connected.',
              style: TextStyle(color: AppColors.textFaint, fontSize: 12)),
        ],
      ),
    );
  }
}
