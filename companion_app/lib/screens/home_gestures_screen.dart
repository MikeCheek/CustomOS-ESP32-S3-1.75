import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';

import '../providers/ble_provider.dart';
import '../providers/home_provider.dart';
import '../theme/app_theme.dart';
import '../widgets/common.dart';

/// What each gesture on the watch face does, and the Tiles: a style to start
/// from, then any gesture changed one by one. Changes show on the watch as
/// you make them.
class HomeGesturesScreen extends ConsumerWidget {
  const HomeGesturesScreen({super.key});

  @override
  Widget build(BuildContext context, WidgetRef ref) {
    final connected = ref.watch(bleProvider).isConnected;
    final known = ref.watch(homeProvider);
    final h = known ?? HomeGestures.defaults;
    final can = connected && known != null;
    final n = ref.read(homeProvider.notifier);
    final style = h.style;

    return Scaffold(
      appBar: AppBar(title: const Text('Home gestures')),
      body: ListView(
        padding: const EdgeInsets.fromLTRB(16, 4, 16, 32),
        children: [
          if (!can)
            Padding(
              padding: const EdgeInsets.only(bottom: 8),
              child: Text(
                !connected
                    ? 'Connect the watch to change its gestures - changes show on it right away.'
                    : 'Update the watch firmware to change its gestures from here.',
                style: const TextStyle(color: AppColors.textDim, fontSize: 13),
              ),
            ),

          // ---- style
          SectionLabel(
            'Style',
            trailing: Text(style?.label ?? 'Custom', style: const TextStyle(color: AppColors.textDim)),
          ),
          RowGroup(
            children: [
              for (final st in homeStyles)
                NavRow(
                  icon: style?.key == st.key ? Icons.radio_button_checked_rounded : Icons.radio_button_off_rounded,
                  color: style?.key == st.key ? AppColors.accent : AppColors.textFaint,
                  title: st.label,
                  subtitle: st.about,
                  trailing: const SizedBox.shrink(),
                  onTap: can ? () => n.setStyle(st) : null,
                ),
            ],
          ),

          // ---- each gesture
          const SectionLabel('On the watch face'),
          RowGroup(
            children: [
              for (final s in homeSlots)
                NavRow(
                  icon: s.icon,
                  color: AppColors.accent2,
                  title: s.label,
                  subtitle: s.hint,
                  trailing: Row(
                    mainAxisSize: MainAxisSize.min,
                    children: [
                      Text(homeAction(h.action(s.key)).label, style: const TextStyle(color: AppColors.textDim)),
                      const Icon(Icons.chevron_right_rounded, color: AppColors.textFaint),
                    ],
                  ),
                  onTap: can ? () => _pick(context, ref, s, h.action(s.key)) : null,
                ),
            ],
          ),
          const Padding(
            padding: EdgeInsets.fromLTRB(4, 8, 4, 0),
            child: Text(
              'Swipes from the edges are on the face only - in apps, a swipe from the left edge is always back, '
              'and pulling down from the top always opens Control Center.',
              style: TextStyle(color: AppColors.textFaint, fontSize: 12),
            ),
          ),

          // ---- tiles
          SectionLabel(
            'Tiles',
            trailing: Text('${h.tiles.length} shown', style: const TextStyle(color: AppColors.textDim)),
          ),
          const Padding(
            padding: EdgeInsets.fromLTRB(4, 0, 4, 8),
            child: Text(
              'Full-screen pages to flick through. Drag to reorder; the first is the one you see first.',
              style: TextStyle(color: AppColors.textDim, fontSize: 13),
            ),
          ),
          _TilesEditor(tiles: h.tiles, enabled: can, onChanged: n.setTiles),
        ],
      ),
    );
  }

  static void _pick(BuildContext context, WidgetRef ref, HomeSlot s, String cur) {
    showModalBottomSheet(
      context: context,
      isScrollControlled: true,
      showDragHandle: true,
      builder: (ctx) => DraggableScrollableSheet(
        expand: false,
        initialChildSize: 0.7,
        maxChildSize: 0.92,
        builder: (ctx, scroll) => ListView(
          controller: scroll,
          children: [
            Padding(
              padding: const EdgeInsets.fromLTRB(20, 0, 20, 8),
              child: Text(s.label, style: const TextStyle(fontSize: 18, fontWeight: FontWeight.w600)),
            ),
            for (final a in actionsFor(s.key))
              ListTile(
                leading: Icon(a.icon, color: a.key == cur ? AppColors.accent : AppColors.textDim),
                title: Text(a.label),
                subtitle: a.key == 'face' ? const Text('The Photo face: the next photo') : null,
                trailing: a.key == cur ? const Icon(Icons.check_rounded, color: AppColors.accent) : null,
                onTap: () {
                  ref.read(homeProvider.notifier).setAction(s.key, a.key);
                  Navigator.pop(ctx);
                },
              ),
          ],
        ),
      ),
    );
  }
}

/// The tiles shown, in order (drag to move), then the others to add.
class _TilesEditor extends StatelessWidget {
  final List<String> tiles;
  final bool enabled;
  final ValueChanged<List<String>> onChanged;
  const _TilesEditor({required this.tiles, required this.enabled, required this.onChanged});

  @override
  Widget build(BuildContext context) {
    final shown = [for (final k in tiles) allTiles.firstWhere((t) => t.key == k)];
    final hidden = [
      for (final t in allTiles)
        if (!tiles.contains(t.key)) t,
    ];
    return Column(
      children: [
        Panel(
          padding: const EdgeInsets.symmetric(vertical: 4),
          child: shown.isEmpty
              ? const Padding(
                  padding: EdgeInsets.all(16),
                  child: Text('No tiles - add some below', style: TextStyle(color: AppColors.textDim)),
                )
              : ReorderableListView(
                  shrinkWrap: true,
                  physics: const NeverScrollableScrollPhysics(),
                  buildDefaultDragHandles: false,
                  onReorderItem: (from, to) {
                    if (!enabled) return;
                    final l = [...tiles];
                    l.insert(to, l.removeAt(from));
                    onChanged(l);
                  },
                  children: [
                    for (var i = 0; i < shown.length; i++)
                      ListTile(
                        key: ValueKey(shown[i].key),
                        leading: IconChip(shown[i].icon, color: AppColors.accent3, size: 36),
                        title: Text(shown[i].label),
                        subtitle: Text(
                          shown[i].about,
                          style: const TextStyle(color: AppColors.textDim, fontSize: 12.5),
                        ),
                        trailing: Row(
                          mainAxisSize: MainAxisSize.min,
                          children: [
                            IconButton(
                              tooltip: 'Hide',
                              icon: const Icon(Icons.remove_circle_outline_rounded, color: AppColors.textDim),
                              onPressed: enabled ? () => onChanged([...tiles]..remove(shown[i].key)) : null,
                            ),
                            ReorderableDragStartListener(
                              index: i,
                              enabled: enabled,
                              child: const Padding(
                                padding: EdgeInsets.all(8),
                                child: Icon(Icons.drag_handle_rounded, color: AppColors.textFaint),
                              ),
                            ),
                          ],
                        ),
                      ),
                  ],
                ),
        ),
        if (hidden.isNotEmpty) ...[
          const SizedBox(height: 10),
          RowGroup(
            children: [
              for (final t in hidden)
                NavRow(
                  icon: t.icon,
                  color: AppColors.textDim,
                  title: t.label,
                  subtitle: t.about,
                  trailing: const Icon(Icons.add_circle_outline_rounded, color: AppColors.accent),
                  onTap: enabled ? () => onChanged([...tiles, t.key]) : null,
                ),
            ],
          ),
        ],
      ],
    );
  }
}
