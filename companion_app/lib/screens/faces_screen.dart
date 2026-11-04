import 'dart:io';
import 'dart:math' as math;

import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';

import '../providers/ble_provider.dart';
import '../providers/home_provider.dart';
import '../providers/look_provider.dart';
import '../providers/photo_faces_provider.dart';
import '../providers/settings_provider.dart';
import '../theme/app_theme.dart';
import '../widgets/common.dart';
import 'home_gestures_screen.dart';
import 'photo_faces_screen.dart';
import 'watch_slots_screen.dart';
import 'watchface/watchface_library.dart';

/// Everything about how the watch looks, in one place: which face it wears,
/// the main face's colour, parts and slots, the display, and the photo and
/// designed faces. Changes show on the watch as you make them.
class FacesScreen extends ConsumerStatefulWidget {
  const FacesScreen({super.key});

  @override
  ConsumerState<FacesScreen> createState() => _FacesScreenState();
}

class _FacesScreenState extends ConsumerState<FacesScreen> {
  double? _dragBrightness;

  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (ref.read(bleProvider).isConnected) ref.read(photoFacesProvider.notifier).refresh();
    });
  }

  @override
  Widget build(BuildContext context) {
    final connected = ref.watch(bleProvider).isConnected;
    final look = ref.watch(lookProvider) ?? const WatchLook();
    final known = ref.watch(lookProvider) != null;
    final notifier = ref.read(lookProvider.notifier);
    final s = ref.watch(settingsProvider);
    final slots = [...s.compSlots];
    while (slots.length < 3) {
      slots.add('none');
    }
    final pf = ref.watch(photoFacesProvider);
    final home = ref.watch(homeProvider);
    final can = connected && known;

    return Scaffold(
      appBar: AppBar(title: const Text('Faces')),
      body: ListView(
        padding: const EdgeInsets.fromLTRB(16, 4, 16, 32),
        children: [
          if (!connected)
            const Padding(
              padding: EdgeInsets.only(bottom: 8),
              child: Text(
                'Connect the watch to change its face - changes show on it right away.',
                style: TextStyle(color: AppColors.textDim, fontSize: 13),
              ),
            )
          else if (!known)
            const Padding(
              padding: EdgeInsets.only(bottom: 8),
              child: Text(
                'Update the watch to firmware 3.5 to pick and style faces from here.',
                style: TextStyle(color: AppColors.textDim, fontSize: 13),
              ),
            ),
          // ---- which face
          const SectionLabel('Wearing'),
          SizedBox(
            height: 156,
            child: ListView.separated(
              scrollDirection: Axis.horizontal,
              itemCount: look.faces.length,
              separatorBuilder: (_, _) => const SizedBox(width: 12),
              itemBuilder: (_, i) => _FaceCard(
                name: look.faces[i],
                selected: i == look.face,
                look: look,
                slots: slots,
                photoName: pf.current.isNotEmpty ? pf.current : (pf.names.isEmpty ? null : pf.names.first),
                onTap: can ? () => notifier.set('wf', i) : null,
              ),
            ),
          ),
          const SizedBox(height: 4),
          const Text(
            'On the watch: touch and hold the face to switch, swipe up for the Smart Stack.',
            style: TextStyle(color: AppColors.textFaint, fontSize: 12),
          ),

          // ---- the main face
          const SectionLabel('Eyes face'),
          Panel(
            child: Column(
              children: [
                Center(
                  child: _FacePreview(kind: 'Eyes', look: look, slots: slots, size: 200),
                ),
                const SizedBox(height: 14),
                Row(
                  mainAxisAlignment: MainAxisAlignment.center,
                  children: [
                    for (var i = 0; i < WatchLook.accents.length; i++)
                      Padding(
                        padding: const EdgeInsets.symmetric(horizontal: 4),
                        child: GestureDetector(
                          onTap: can ? () => notifier.set('acc', i) : null,
                          child: Container(
                            width: 30,
                            height: 30,
                            decoration: BoxDecoration(
                              color: Color(WatchLook.accents[i]),
                              shape: BoxShape.circle,
                              border: Border.all(
                                color: i == look.accent ? AppColors.text : Colors.transparent,
                                width: 3,
                              ),
                            ),
                          ),
                        ),
                      ),
                  ],
                ),
              ],
            ),
          ),
          const SizedBox(height: 10),
          RowGroup(
            children: [
              ToggleRow(
                icon: Icons.donut_large_rounded,
                title: 'Seconds round the edge',
                value: look.seconds,
                onChanged: can ? (v) => notifier.set('sec', v) : null,
              ),
              ToggleRow(
                icon: Icons.visibility_outlined,
                title: 'Eyes',
                subtitle: 'They follow your finger and blink',
                value: look.eyes,
                onChanged: can ? (v) => notifier.set('eyes', v) : null,
              ),
              ToggleRow(
                icon: Icons.calendar_today_outlined,
                title: 'Date',
                value: look.date,
                onChanged: can ? (v) => notifier.set('date', v) : null,
              ),
              ToggleRow(
                icon: Icons.pets_outlined,
                title: 'Pet',
                subtitle: 'Walks across the faces (pick it in the watch\'s Pet app)',
                value: look.pet,
                onChanged: can ? (v) => notifier.set('pet', v) : null,
              ),
              ToggleRow(
                icon: Icons.cloud_outlined,
                title: 'Weather behind the time',
                subtitle: 'Rain, snow, sun... from the forecast',
                value: look.weatherFx,
                onChanged: can ? (v) => notifier.set('wx', v) : null,
              ),
            ],
          ),

          // ---- slots
          SectionLabel(
            'Slots',
            trailing: Text('Eyes and Minimal faces', style: const TextStyle(color: AppColors.textFaint, fontSize: 12)),
          ),
          Row(
            children: [
              for (var i = 0; i < 3; i++) ...[
                if (i > 0) const SizedBox(width: 10),
                Expanded(
                  child: _SlotTile(index: i, value: slots[i], slots: slots),
                ),
              ],
            ],
          ),

          // ---- display
          const SectionLabel('Display'),
          Panel(
            padding: const EdgeInsets.fromLTRB(16, 12, 16, 8),
            child: Row(
              children: [
                const Icon(Icons.brightness_6_outlined, color: AppColors.textDim),
                Expanded(
                  child: Slider(
                    value: (_dragBrightness ?? look.brightness.toDouble()).clamp(8, 255),
                    min: 8,
                    max: 255,
                    onChanged: can ? (v) => setState(() => _dragBrightness = v) : null,
                    onChangeEnd: can
                        ? (v) {
                            notifier.set('br', v.round());
                            setState(() => _dragBrightness = null);
                          }
                        : null,
                  ),
                ),
                SizedBox(
                  width: 44,
                  child: Text(
                    '${((_dragBrightness ?? look.brightness.toDouble()) * 100 / 255).round()}%',
                    textAlign: TextAlign.right,
                    style: const TextStyle(color: AppColors.textDim),
                  ),
                ),
              ],
            ),
          ),
          const SizedBox(height: 10),
          RowGroup(
            children: [
              ToggleRow(
                icon: Icons.nightlight_outlined,
                title: 'Always-on display',
                subtitle: 'A dim clock while the watch sleeps',
                value: look.alwaysOn,
                onChanged: can ? (v) => notifier.set('aod', v) : null,
              ),
              ToggleRow(
                icon: Icons.schedule_rounded,
                title: '24-hour clock',
                value: look.h24,
                onChanged: can ? (v) => notifier.set('h24', v) : null,
              ),
              ToggleRow(
                icon: Icons.view_list_rounded,
                title: 'Apps as a list',
                subtitle: 'A-Z instead of the honeycomb',
                value: look.menuList,
                onChanged: can ? (v) => notifier.set('ml', v) : null,
              ),
            ],
          ),
          const SizedBox(height: 10),
          Panel(
            padding: const EdgeInsets.fromLTRB(16, 12, 16, 12),
            child: Row(
              children: [
                const Icon(Icons.format_size_rounded, color: AppColors.textDim),
                const SizedBox(width: 14),
                const Expanded(child: Text('Text size')),
                SegmentedButton<int>(
                  segments: const [
                    ButtonSegment(value: 0, label: Text('A', style: TextStyle(fontSize: 12))),
                    ButtonSegment(value: 1, label: Text('A', style: TextStyle(fontSize: 15))),
                    ButtonSegment(value: 2, label: Text('A', style: TextStyle(fontSize: 18))),
                  ],
                  selected: {look.textSize},
                  showSelectedIcon: false,
                  onSelectionChanged: can ? (v) => notifier.set('ts', v.first) : null,
                ),
              ],
            ),
          ),

          // ---- gestures on the face
          const SectionLabel('Home gestures'),
          RowGroup(
            children: [
              NavRow(
                icon: Icons.swipe_rounded,
                color: AppColors.accent2,
                title: 'Gestures and Tiles',
                subtitle: home == null
                    ? 'Tap, swipes, BOOT and the Tiles on the face'
                    : '${home.style?.label ?? 'Custom'} - tap: ${homeAction(home.action('tap')).label}, '
                          'from the right: ${homeAction(home.action('left')).label}',
                onTap: () => Navigator.push(context, MaterialPageRoute(builder: (_) => const HomeGesturesScreen())),
              ),
            ],
          ),

          // ---- more faces
          const SectionLabel('More faces'),
          RowGroup(
            children: [
              NavRow(
                icon: Icons.photo_library_outlined,
                color: AppColors.accent3,
                title: 'Photo faces',
                subtitle: pf.names.isEmpty
                    ? 'Your photos, the time behind the person or pet'
                    : '${pf.names.length} on the watch',
                onTap: () => Navigator.push(context, MaterialPageRoute(builder: (_) => const PhotoFaceScreen())),
              ),
              NavRow(
                icon: Icons.dashboard_customize_outlined,
                color: AppColors.accent2,
                title: 'Design a face',
                subtitle: 'Place the time, date, rings and text yourself (the Custom face)',
                onTap: () => Navigator.push(context, MaterialPageRoute(builder: (_) => const WatchfaceLibrary())),
              ),
            ],
          ),
        ],
      ),
    );
  }
}

/// One of the watch's faces in the "Wearing" row.
class _FaceCard extends StatelessWidget {
  final String name;
  final bool selected;
  final WatchLook look;
  final List<String> slots;
  final String? photoName;
  final VoidCallback? onTap;
  const _FaceCard({
    required this.name,
    required this.selected,
    required this.look,
    required this.slots,
    this.photoName,
    this.onTap,
  });

  @override
  Widget build(BuildContext context) {
    return GestureDetector(
      onTap: onTap,
      child: Column(
        children: [
          Container(
            padding: const EdgeInsets.all(3),
            decoration: BoxDecoration(
              shape: BoxShape.circle,
              border: Border.all(color: selected ? AppColors.accent : AppColors.surfaceHigh, width: 3),
            ),
            child: _FacePreview(kind: name, look: look, slots: slots, size: 110, photoName: photoName),
          ),
          const SizedBox(height: 8),
          Text(
            name,
            style: TextStyle(
              color: selected ? AppColors.text : AppColors.textDim,
              fontWeight: selected ? FontWeight.w600 : FontWeight.w400,
            ),
          ),
        ],
      ),
    );
  }
}

/// A small drawing of a face as the watch shows it, following the look.
class _FacePreview extends StatelessWidget {
  final String kind;
  final WatchLook look;
  final List<String> slots;
  final double size;
  final String? photoName;
  const _FacePreview({required this.kind, required this.look, required this.slots, required this.size, this.photoName});

  @override
  Widget build(BuildContext context) {
    final accent = Color(WatchLook.accents[look.accent % WatchLook.accents.length]);
    final t = TimeOfDay.now();
    final h = look.h24 ? t.hour : (t.hour % 12 == 0 ? 12 : t.hour % 12);
    final time = '${look.h24 ? h.toString().padLeft(2, '0') : '$h'}:${t.minute.toString().padLeft(2, '0')}';
    final k = size / 200;
    Widget body;
    if (kind == 'Photo' && photoName != null) {
      body = FutureBuilder<File>(
        future: PhotoFacesNotifier.thumbFile(photoName!),
        builder: (_, snap) => snap.hasData && snap.data!.existsSync()
            ? Image.file(snap.data!, fit: BoxFit.cover, width: size, height: size)
            : const SizedBox.shrink(),
      );
    } else {
      body = const SizedBox.shrink();
    }
    return ClipOval(
      child: Container(
        width: size,
        height: size,
        color: Colors.black,
        child: Stack(
          alignment: Alignment.center,
          children: [
            Positioned.fill(child: body),
            if (kind == 'Eyes' && look.seconds)
              Positioned.fill(child: CustomPaint(painter: _SecondsArc(accent, DateTime.now().second / 60))),
            if (kind == 'Eyes' && look.eyes)
              Positioned(
                top: 38 * k,
                child: Row(
                  children: [
                    for (var i = 0; i < 2; i++) ...[
                      if (i > 0) SizedBox(width: 8 * k),
                      Container(
                        width: 22 * k,
                        height: 26 * k,
                        decoration: BoxDecoration(color: Colors.white, borderRadius: BorderRadius.circular(7 * k)),
                        alignment: Alignment.center,
                        child: Container(
                          width: 10 * k,
                          height: 10 * k,
                          decoration: BoxDecoration(color: accent, shape: BoxShape.circle),
                        ),
                      ),
                    ],
                  ],
                ),
              ),
            if (kind == 'Custom')
              Icon(Icons.dashboard_customize_outlined, color: AppColors.accent2.withValues(alpha: 0.5), size: 60 * k),
            if (kind == 'Photo' && photoName == null)
              Icon(Icons.photo_outlined, color: AppColors.textFaint, size: 60 * k),
            Positioned(
              top:
                  (kind == 'Eyes'
                      ? 72
                      : kind == 'Photo'
                      ? 36
                      : 62) *
                  k,
              child: Text(
                time,
                style: TextStyle(
                  color: Colors.white,
                  fontSize: (kind == 'Minimal' ? 46 : 40) * k,
                  fontWeight: kind == 'Minimal' ? FontWeight.w300 : FontWeight.w700,
                  shadows: kind == 'Photo' ? const [Shadow(blurRadius: 6)] : null,
                ),
              ),
            ),
            if ((kind == 'Eyes' && look.date) || kind == 'Minimal')
              Positioned(
                top: (kind == 'Eyes' ? 120 : 112) * k,
                child: Text(
                  MaterialLocalizations.of(context).formatShortMonthDay(DateTime.now()),
                  style: TextStyle(color: AppColors.textDim, fontSize: 13 * k),
                ),
              ),
            if (kind == 'Eyes' || kind == 'Minimal')
              Positioned(
                bottom: 22 * k,
                child: Row(
                  children: [
                    for (var i = 0; i < 3; i++) ...[
                      if (i > 0) SizedBox(width: 6 * k),
                      Container(
                        width: 34 * k,
                        height: 34 * k,
                        decoration: BoxDecoration(
                          shape: BoxShape.circle,
                          border: Border.all(
                            color: accent.withValues(alpha: slots[i] == 'none' ? 0.15 : 0.85),
                            width: math.max(1.5, 3 * k),
                          ),
                        ),
                        child: Icon(
                          WatchSlotsScreen.options[slots[i]]?.$2 ?? Icons.help_outline,
                          size: 15 * k,
                          color: Colors.white,
                        ),
                      ),
                    ],
                  ],
                ),
              ),
          ],
        ),
      ),
    );
  }
}

class _SecondsArc extends CustomPainter {
  final Color color;
  final double frac;
  _SecondsArc(this.color, this.frac);
  @override
  void paint(Canvas canvas, Size size) {
    final r = Rect.fromLTWH(4, 4, size.width - 8, size.height - 8);
    canvas.drawArc(
      r,
      -math.pi / 2,
      2 * math.pi * math.max(frac, 0.05),
      false,
      Paint()
        ..color = color
        ..style = PaintingStyle.stroke
        ..strokeWidth = size.width * 0.025,
    );
  }

  @override
  bool shouldRepaint(_SecondsArc o) => o.color != color || o.frac != frac;
}

/// One complication slot: tap to choose what it shows.
class _SlotTile extends ConsumerWidget {
  final int index;
  final String value;
  final List<String> slots;
  const _SlotTile({required this.index, required this.value, required this.slots});

  static const _names = ['Left', 'Middle', 'Right'];

  @override
  Widget build(BuildContext context, WidgetRef ref) {
    final opt = WatchSlotsScreen.options[value] ?? WatchSlotsScreen.options['none']!;
    return Panel(
      padding: EdgeInsets.zero,
      child: InkWell(
        borderRadius: BorderRadius.circular(18),
        onTap: () async {
          final v = await showModalBottomSheet<String>(
            context: context,
            builder: (ctx) => SafeArea(
              child: ListView(
                shrinkWrap: true,
                children: [
                  Padding(
                    padding: const EdgeInsets.fromLTRB(20, 16, 20, 8),
                    child: Text(
                      '${_names[index]} slot',
                      style: const TextStyle(color: AppColors.text, fontSize: 17, fontWeight: FontWeight.w600),
                    ),
                  ),
                  for (final e in WatchSlotsScreen.options.entries)
                    ListTile(
                      leading: Icon(e.value.$2, color: AppColors.textDim),
                      title: Text(e.value.$1),
                      trailing: e.key == value ? const Icon(Icons.check_rounded, color: AppColors.accent) : null,
                      onTap: () => Navigator.pop(ctx, e.key),
                    ),
                ],
              ),
            ),
          );
          if (v == null) return;
          final s = ref.read(settingsProvider);
          ref.read(settingsProvider.notifier).update(s.copyWith(compSlots: [...slots]..[index] = v));
        },
        child: Padding(
          padding: const EdgeInsets.symmetric(vertical: 14, horizontal: 8),
          child: Column(
            children: [
              Icon(opt.$2, color: value == 'none' ? AppColors.textFaint : AppColors.accent),
              const SizedBox(height: 6),
              Text(opt.$1, textAlign: TextAlign.center, maxLines: 2, style: const TextStyle(fontSize: 12.5)),
              const SizedBox(height: 2),
              Text(_names[index], style: const TextStyle(color: AppColors.textFaint, fontSize: 11)),
            ],
          ),
        ),
      ),
    );
  }
}
