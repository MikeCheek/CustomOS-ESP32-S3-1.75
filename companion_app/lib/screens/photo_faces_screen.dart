import 'dart:io';

import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';

import '../providers/ble_provider.dart';
import '../providers/photo_faces_provider.dart';
import '../theme/app_theme.dart';
import '../widgets/common.dart';
import 'photo_face_screen.dart';

/// The photo faces on the watch: which one shows, how they change, add and
/// delete. Each one is made in PhotoFaceEditor.
class PhotoFaceScreen extends ConsumerStatefulWidget {
  const PhotoFaceScreen({super.key});

  @override
  ConsumerState<PhotoFaceScreen> createState() => _PhotoFaceScreenState();
}

class _PhotoFaceScreenState extends ConsumerState<PhotoFaceScreen> {
  static const _modes = ['Tap to change', 'Every hour', 'Every day'];

  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addPostFrameCallback((_) => _refresh());
  }

  Future<void> _refresh() async {
    if (ref.read(bleProvider).isConnected) await ref.read(photoFacesProvider.notifier).refresh();
  }

  Future<void> _add() async {
    await Navigator.push<bool>(context, MaterialPageRoute(builder: (_) => const PhotoFaceEditor()));
    if (mounted) setState(() {}); // a new thumbnail
  }

  Future<void> _delete(String name) async {
    final ok = await showDialog<bool>(
      context: context,
      builder: (ctx) => AlertDialog(
        title: const Text('Delete this photo face?'),
        content: const Text('It\'s removed from the watch\'s SD card.'),
        actions: [
          TextButton(onPressed: () => Navigator.pop(ctx, false), child: const Text('Cancel')),
          TextButton(
            onPressed: () => Navigator.pop(ctx, true),
            child: const Text('Delete', style: TextStyle(color: AppColors.danger)),
          ),
        ],
      ),
    );
    if (ok == true) await ref.read(photoFacesProvider.notifier).delete(name);
  }

  @override
  Widget build(BuildContext context) {
    final connected = ref.watch(bleProvider).isConnected;
    final pf = ref.watch(photoFacesProvider);
    final notifier = ref.read(photoFacesProvider.notifier);
    final full = pf.names.length >= pf.max;

    return Scaffold(
      appBar: AppBar(title: const Text('Photo faces')),
      floatingActionButton: connected && !full
          ? FloatingActionButton.extended(
              onPressed: _add,
              icon: const Icon(Icons.add_photo_alternate_outlined),
              label: const Text('Add a photo'),
            )
          : null,
      body: RefreshIndicator(
        onRefresh: _refresh,
        child: ListView(
          padding: const EdgeInsets.fromLTRB(16, 8, 16, 96),
          children: [
            const Text(
              'Your photos as the watch face, the time behind the person, pet or thing in each. '
              'Tap the face on the watch for the next one.',
              style: TextStyle(color: AppColors.textDim, fontSize: 13, height: 1.4),
            ),
            if (!connected) ...[
              const SizedBox(height: 24),
              const EmptyState(
                icon: Icons.watch_off_outlined,
                title: 'Connect the watch',
                subtitle: 'The photos live on its SD card. The watch emulator works too.',
              ),
            ] else if (!pf.known) ...[
              const SizedBox(height: 48),
              const Center(child: CircularProgressIndicator()),
            ] else if (pf.names.isEmpty) ...[
              const SizedBox(height: 24),
              EmptyState(
                icon: Icons.portrait_rounded,
                title: 'No photo faces yet',
                subtitle: 'Add a photo: the phone finds its subject and puts the time behind it.',
                action: FilledButton.icon(
                  onPressed: _add,
                  icon: const Icon(Icons.add_photo_alternate_outlined),
                  label: const Text('Add a photo'),
                ),
              ),
            ] else ...[
              const SectionLabel('Change photo'),
              SegmentedButton<int>(
                segments: [for (var i = 0; i < _modes.length; i++) ButtonSegment(value: i, label: Text(_modes[i]))],
                selected: {pf.rotate},
                showSelectedIcon: false,
                onSelectionChanged: (s) => notifier.setRotate(s.first),
              ),
              SectionLabel(
                'On the watch',
                trailing: Text(
                  '${pf.names.length} of ${pf.max}',
                  style: const TextStyle(color: AppColors.textDim, fontSize: 12),
                ),
              ),
              LayoutBuilder(
                builder: (context, c) {
                  final cols = c.maxWidth > 520 ? 4 : 3;
                  final w = (c.maxWidth - 12 * (cols - 1)) / cols;
                  return Wrap(
                    spacing: 12,
                    runSpacing: 14,
                    children: [
                      for (final n in pf.names)
                        SizedBox(
                          width: w,
                          child: _FaceTile(
                            name: n,
                            current: n == pf.current,
                            onTap: () => notifier.show(n),
                            onDelete: () => _delete(n),
                          ),
                        ),
                    ],
                  );
                },
              ),
              if (full) ...[
                const SizedBox(height: 16),
                Text(
                  'The watch holds ${pf.max}: delete one to add another.',
                  textAlign: TextAlign.center,
                  style: const TextStyle(color: AppColors.warning, fontSize: 12),
                ),
              ],
            ],
          ],
        ),
      ),
    );
  }
}

class _FaceTile extends StatelessWidget {
  final String name;
  final bool current;
  final VoidCallback onTap, onDelete;
  const _FaceTile({required this.name, required this.current, required this.onTap, required this.onDelete});

  @override
  Widget build(BuildContext context) {
    return Column(
      children: [
        GestureDetector(
          onTap: onTap,
          onLongPress: onDelete,
          child: AspectRatio(
            aspectRatio: 1,
            child: Container(
              padding: const EdgeInsets.all(3),
              decoration: BoxDecoration(
                shape: BoxShape.circle,
                border: Border.all(color: current ? AppColors.accent : AppColors.border, width: current ? 3 : 1),
              ),
              child: ClipOval(
                child: FutureBuilder<File>(
                  future: PhotoFacesNotifier.thumbFile(name),
                  builder: (context, snap) {
                    final f = snap.data;
                    if (f != null && f.existsSync()) return Image.file(f, fit: BoxFit.cover);
                    return const ColoredBox(
                      color: AppColors.surfaceLight,
                      child: Icon(Icons.portrait_rounded, color: AppColors.textDim),
                    );
                  },
                ),
              ),
            ),
          ),
        ),
        const SizedBox(height: 6),
        Row(
          mainAxisAlignment: MainAxisAlignment.center,
          children: [
            Flexible(
              child: Text(
                current ? 'Showing' : _label(name),
                overflow: TextOverflow.ellipsis,
                style: TextStyle(
                  color: current ? AppColors.accent : AppColors.textDim,
                  fontSize: 12,
                  fontWeight: current ? FontWeight.w600 : FontWeight.w400,
                ),
              ),
            ),
            InkWell(
              onTap: onDelete,
              borderRadius: BorderRadius.circular(12),
              child: const Padding(
                padding: EdgeInsets.all(4),
                child: Icon(Icons.delete_outline_rounded, size: 16, color: AppColors.textFaint),
              ),
            ),
          ],
        ),
      ],
    );
  }

  // "p20261103154512" -> "3 Nov"; anything else as it is
  static String _label(String n) {
    final m = RegExp(r'^p(\d{4})(\d{2})(\d{2})').firstMatch(n);
    if (m == null) return n;
    const months = ['Jan', 'Feb', 'Mar', 'Apr', 'May', 'Jun', 'Jul', 'Aug', 'Sep', 'Oct', 'Nov', 'Dec'];
    final mo = int.parse(m.group(2)!);
    return '${int.parse(m.group(3)!)} ${mo >= 1 && mo <= 12 ? months[mo - 1] : ''}';
  }
}
