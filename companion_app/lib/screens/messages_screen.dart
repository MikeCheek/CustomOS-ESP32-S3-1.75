import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';
import '../providers/ble_provider.dart';
import '../services/notif_ai.dart';
import '../theme/app_theme.dart';
import '../widgets/common.dart';

/// Recent notifications with the on-phone AI: a "Catch me up" summary,
/// translation and smart replies - the same as on the watch.
class MessagesScreen extends ConsumerStatefulWidget {
  const MessagesScreen({super.key});

  @override
  ConsumerState<MessagesScreen> createState() => _MessagesScreenState();
}

class _MessagesScreenState extends ConsumerState<MessagesScreen> {
  final _ai = NotifAi.instance;
  String? _digest;
  bool _digesting = false;
  final Set<int> _busy = {};

  Future<void> _catchUp() async {
    setState(() => _digesting = true);
    try {
      _digest = await _ai.digest();
    } catch (e) {
      if (mounted) showSnack(context, e.toString().replaceFirst('Exception: ', ''));
    } finally {
      if (mounted) setState(() => _digesting = false);
    }
  }

  Future<void> _run(int id, Future<void> Function() f) async {
    setState(() => _busy.add(id));
    try {
      await f();
    } catch (e) {
      if (mounted) showSnack(context, e.toString().replaceFirst('Exception: ', ''));
    } finally {
      if (mounted) setState(() => _busy.remove(id));
    }
  }

  Future<void> _reply(LoggedNotif n, String text) async {
    final ok = await ref.read(nativeServiceProvider).notificationAction(n.id, 'reply', text: text);
    if (mounted) showSnack(context, ok ? 'Sent: $text' : 'That message can\'t be answered any more');
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(title: const Text('Messages')),
      body: StreamBuilder<void>(
        stream: _ai.changes,
        builder: (context, _) {
          final list = _ai.recent;
          return ListView(
            padding: const EdgeInsets.fromLTRB(16, 8, 16, 32),
            children: [
              Panel(
                child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
                  Row(children: [
                    const Icon(Icons.auto_awesome_rounded, color: AppColors.accent2),
                    const SizedBox(width: 12),
                    const Expanded(
                        child: Text('Catch me up',
                            style: TextStyle(color: AppColors.text, fontSize: 15, fontWeight: FontWeight.w600))),
                    if (_digesting)
                      const SizedBox(width: 22, height: 22, child: CircularProgressIndicator(strokeWidth: 2))
                    else
                      TextButton(onPressed: list.isEmpty ? null : _catchUp, child: const Text('Summarize')),
                  ]),
                  if (_digest != null) ...[
                    const SizedBox(height: 8),
                    Text(_digest!, style: const TextStyle(color: AppColors.text, height: 1.45)),
                  ],
                ]),
              ),
              const SectionLabel('Recent'),
              if (list.isEmpty)
                const Text(
                  'Notifications appear here while the app has notification access (Settings > Notifications).',
                  style: TextStyle(color: AppColors.textDim, height: 1.4),
                ),
              for (final n in list) ...[
                Panel(
                  child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
                    Text(n.app, style: const TextStyle(color: AppColors.accent, fontSize: 12)),
                    if (n.title.isNotEmpty)
                      Text(n.title, style: const TextStyle(color: AppColors.text, fontWeight: FontWeight.w600)),
                    const SizedBox(height: 2),
                    Text(n.text, style: const TextStyle(color: AppColors.textDim, height: 1.35)),
                    if (n.translation != null) ...[
                      const SizedBox(height: 6),
                      Text(n.translation!, style: const TextStyle(color: AppColors.accent2, height: 1.35)),
                    ],
                    if (n.replies.isNotEmpty && n.canReply) ...[
                      const SizedBox(height: 8),
                      Wrap(spacing: 8, runSpacing: 6, children: [
                        for (final r in n.replies) ActionChip(label: Text(r), onPressed: () => _reply(n, r)),
                      ]),
                    ],
                    Row(children: [
                      const Spacer(),
                      if (_busy.contains(n.id))
                        const Padding(
                          padding: EdgeInsets.all(10),
                          child: SizedBox(width: 18, height: 18, child: CircularProgressIndicator(strokeWidth: 2)),
                        )
                      else ...[
                        if (n.translation == null)
                          TextButton(onPressed: () => _run(n.id, () => _ai.translate(n.id)), child: const Text('Translate')),
                        if (n.canReply && n.replies.isEmpty)
                          TextButton(
                              onPressed: () => _run(n.id, () => _ai.repliesFor(n.id)), child: const Text('Suggest replies')),
                      ],
                    ]),
                  ]),
                ),
                const SizedBox(height: 10),
              ],
            ],
          );
        },
      ),
    );
  }
}
