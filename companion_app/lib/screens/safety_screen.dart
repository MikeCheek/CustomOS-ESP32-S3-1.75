import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:permission_handler/permission_handler.dart';
import '../providers/ble_provider.dart';
import '../services/safety.dart';
import '../theme/app_theme.dart';
import '../widgets/common.dart';
import '../widgets/platform_notice.dart';

/// Emergency contact for the watch's fall detection.
class SafetyScreen extends ConsumerStatefulWidget {
  const SafetyScreen({super.key});

  @override
  ConsumerState<SafetyScreen> createState() => _SafetyScreenState();
}

class _SafetyScreenState extends ConsumerState<SafetyScreen> {
  final _name = TextEditingController(), _phone = TextEditingController();

  @override
  void initState() {
    super.initState();
    Safety.contact().then((c) {
      if (!mounted) return;
      _name.text = c.name;
      _phone.text = c.phone;
      setState(() {});
    });
  }

  @override
  void dispose() {
    _name.dispose();
    _phone.dispose();
    super.dispose();
  }

  Future<void> _pick() async {
    if (!(await Permission.contacts.request()).isGranted) {
      if (mounted) showSnack(context, 'Contacts access is needed to pick one');
      return;
    }
    final all = await ref.read(nativeServiceProvider).getContacts();
    if (!mounted) return;
    final picked = await showModalBottomSheet<Map<String, String>>(
      context: context,
      backgroundColor: AppColors.surfaceLight,
      builder: (ctx) => ListView(
        children: [
          for (final c in all.where((c) => (c['phone'] ?? '').isNotEmpty))
            ListTile(
              title: Text(c['name'] ?? '', style: const TextStyle(color: AppColors.text)),
              subtitle: Text(c['phone'] ?? '', style: const TextStyle(color: AppColors.textDim)),
              onTap: () => Navigator.pop(ctx, c),
            ),
        ],
      ),
    );
    if (picked == null) return;
    _name.text = picked['name'] ?? '';
    _phone.text = picked['phone'] ?? '';
    await _save();
  }

  Future<bool> _save() async {
    await Safety.setContact(_name.text, _phone.text);
    if (_phone.text.trim().isEmpty) return false;
    // iPhone: no texting by itself - a notification opens Messages with the text ready
    final ok = (await (isIos ? Permission.notification : Permission.sms).request()).isGranted;
    if (mounted) {
      showSnack(
        context,
        ok
            ? 'Saved'
            : isIos
            ? 'Saved - allow notifications so the phone can show the fall alert'
            : 'Saved - allow SMS so the phone can text them after a fall',
      );
      setState(() {});
    }
    return ok;
  }

  Future<void> _test() async {
    if (!await _save()) return;
    final native = ref.read(nativeServiceProvider);
    final loc = await native.lastLocation();
    try {
      await native.sendSms(
        (await Safety.contact()).phone,
        Safety.alertText(DateTime.now(), lat: loc?['lat'], lon: loc?['lon'], test: true),
      );
      if (mounted) showSnack(context, isIos ? 'Messages opened with the test text' : 'Test text sent');
    } on PlatformException catch (e) {
      if (mounted) showSnack(context, 'Couldn\'t send it: ${e.message ?? e.code}');
    }
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(title: const Text('Safety')),
      body: ListView(
        padding: const EdgeInsets.fromLTRB(16, 8, 16, 32),
        children: [
          Text(
            isIos
                ? 'With Fall detection on (watch: Settings > Wake & quiet), the watch asks "Did you fall?" after a hard '
                      'fall. If nobody answers in 30 seconds, this phone rings and Messages opens with a text to your '
                      'emergency contact, with the last known location - iOS doesn\'t let apps send a text on their own, '
                      'so it takes one tap on Send. With the app in the background, a notification appears instead: tap '
                      'it, then Send. It works while the watch is connected to this phone.'
                : 'With Fall detection on (watch: Settings > Wake & quiet), the watch asks "Did you fall?" after a hard '
                      'fall. If nobody answers in 30 seconds, this phone rings and texts your emergency contact, with the '
                      'last known location. It works while the watch is connected to this phone.',
            style: const TextStyle(color: AppColors.textDim, fontSize: 13, height: 1.4),
          ),
          const SectionLabel('Emergency contact'),
          Panel(
            child: Column(
              children: [
                TextField(
                  controller: _name,
                  decoration: const InputDecoration(labelText: 'Name'),
                ),
                const SizedBox(height: 8),
                TextField(
                  controller: _phone,
                  keyboardType: TextInputType.phone,
                  decoration: const InputDecoration(labelText: 'Phone number'),
                  onSubmitted: (_) => _save(),
                ),
                const SizedBox(height: 14),
                Row(
                  children: [
                    OutlinedButton.icon(
                      onPressed: _pick,
                      icon: const Icon(Icons.contacts_outlined, size: 18),
                      label: const Text('Pick'),
                    ),
                    const SizedBox(width: 8),
                    OutlinedButton(onPressed: _save, child: const Text('Save')),
                    const Spacer(),
                    TextButton(onPressed: _test, child: Text(isIos ? 'Try it' : 'Send a test')),
                  ],
                ),
              ],
            ),
          ),
        ],
      ),
    );
  }
}
