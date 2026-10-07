import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:permission_handler/permission_handler.dart';
import '../providers/ble_provider.dart';
import '../providers/companion_provider.dart';
import '../providers/settings_provider.dart';
import '../theme/app_theme.dart';
import '../widgets/common.dart';

/// Calendar on the watch: on/off, reminder time, and what the watch has.
class CalendarScreen extends ConsumerStatefulWidget {
  const CalendarScreen({super.key});

  @override
  ConsumerState<CalendarScreen> createState() => _CalendarScreenState();
}

class _CalendarScreenState extends ConsumerState<CalendarScreen> {
  static const _leads = [0, 5, 10, 15, 30];

  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addPostFrameCallback((_) => ref.read(companionProvider.notifier).refreshCalendar());
  }

  Future<void> _allow() async {
    final st = await Permission.calendarFullAccess.request();
    if (st.isPermanentlyDenied) {
      await openAppSettings();
      return;
    }
    if (st.isGranted) await ref.read(companionProvider.notifier).calendarPermissionChanged();
  }

  Future<void> _set(AppSettings s) async {
    await ref.read(settingsProvider.notifier).update(s);
    await ref.read(companionProvider.notifier).refreshCalendar();
  }

  @override
  Widget build(BuildContext context) {
    final s = ref.watch(settingsProvider);
    final link = ref.watch(companionProvider);
    final connected = ref.watch(bleProvider).isConnected;

    return Scaffold(
      appBar: AppBar(title: const Text('Calendar')),
      body: ListView(
        padding: const EdgeInsets.fromLTRB(16, 0, 16, 32),
        children: [
          if (!link.calendarAllowed) ...[
            const SizedBox(height: 8),
            Panel(
              child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
                const Row(children: [
                  IconChip(Icons.event_rounded, color: AppColors.warning),
                  SizedBox(width: 14),
                  Expanded(
                    child: Text('Allow calendar access',
                        style: TextStyle(color: AppColors.text, fontSize: 16, fontWeight: FontWeight.w600)),
                  ),
                ]),
                const SizedBox(height: 10),
                const Text(
                  'The watch shows your next two days and reminds you before each event - even when the '
                  'phone is out of range. The app only reads your calendar.',
                  style: TextStyle(color: AppColors.textDim, fontSize: 14, height: 1.4),
                ),
                const SizedBox(height: 14),
                SizedBox(width: double.infinity, child: FilledButton(onPressed: _allow, child: const Text('Allow'))),
              ]),
            ),
          ],
          const SectionLabel('On the watch'),
          RowGroup(children: [
            ToggleRow(
              icon: Icons.calendar_month_rounded,
              color: AppColors.accent,
              title: 'Agenda on the watch',
              subtitle: 'Next 48 hours, updated when your calendar changes',
              value: s.calendarToWatch,
              onChanged: (v) => _set(s.copyWith(calendarToWatch: v)),
            ),
          ]),
          const SectionLabel('Reminders'),
          Panel(
            child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
              const Text('Buzz on the watch before an event',
                  style: TextStyle(color: AppColors.text, fontSize: 15, fontWeight: FontWeight.w600)),
              const SizedBox(height: 12),
              Wrap(spacing: 8, runSpacing: 8, children: [
                for (final m in _leads)
                  ChoiceChip(
                    label: Text(m == 0 ? 'Off' : '$m min'),
                    selected: s.reminderMinutes == m,
                    onSelected: s.calendarToWatch ? (_) => _set(s.copyWith(reminderMinutes: m)) : null,
                  ),
              ]),
            ]),
          ),
          SectionLabel(connected ? 'Coming up (sent to the watch)' : 'Coming up'),
          if (link.agenda.isEmpty)
            Panel(
              child: Text(
                link.calendarAllowed ? 'Nothing in the next two days' : 'Calendar access not allowed yet',
                style: const TextStyle(color: AppColors.textDim, fontSize: 14),
              ),
            )
          else
            RowGroup(children: [for (final e in link.agenda) _EventRow(e)]),
        ],
      ),
    );
  }
}

class _EventRow extends StatelessWidget {
  final CalendarItem e;
  const _EventRow(this.e);

  String _when() {
    final now = DateTime.now();
    final today = DateTime(now.year, now.month, now.day);
    final d = DateTime(e.start.year, e.start.month, e.start.day);
    final day = !d.isAfter(today)
        ? 'Today'
        : d == DateTime(today.year, today.month, today.day + 1)
            ? 'Tomorrow'
            : const ['Mon', 'Tue', 'Wed', 'Thu', 'Fri', 'Sat', 'Sun'][d.weekday - 1];
    if (e.allDay) return '$day · all day';
    String hm(DateTime t) => '${t.hour.toString().padLeft(2, '0')}:${t.minute.toString().padLeft(2, '0')}';
    return '$day · ${hm(e.start)}–${hm(e.end)}';
  }

  @override
  Widget build(BuildContext context) {
    final color = e.color == 0 ? AppColors.accent : Color(0xFF000000 | e.color);
    return Padding(
      padding: const EdgeInsets.symmetric(horizontal: 14, vertical: 12),
      child: Row(children: [
        Container(width: 5, height: 40, decoration: BoxDecoration(color: color, borderRadius: BorderRadius.circular(3))),
        const SizedBox(width: 14),
        Expanded(
          child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
            Text(e.title.isEmpty ? '(No title)' : e.title,
                maxLines: 1,
                overflow: TextOverflow.ellipsis,
                style: const TextStyle(color: AppColors.text, fontSize: 15, fontWeight: FontWeight.w600)),
            const SizedBox(height: 2),
            Text(
              e.location.isEmpty ? _when() : '${_when()} · ${e.location}',
              maxLines: 1,
              overflow: TextOverflow.ellipsis,
              style: const TextStyle(color: AppColors.textDim, fontSize: 13),
            ),
          ]),
        ),
      ]),
    );
  }
}
