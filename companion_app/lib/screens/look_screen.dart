import 'dart:io';
import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:image_picker/image_picker.dart';
import 'package:path_provider/path_provider.dart';
import '../providers/ble_provider.dart';
import '../services/gemma_service.dart';
import '../services/memo_player.dart';
import '../services/notif_ai.dart';
import '../services/tts_service.dart';
import '../theme/app_theme.dart';
import '../widgets/common.dart';

/// "What am I looking at?": a photo, and the on-phone vision model
/// (FastVLM or Gemma 3n, Transcription & AI) says what it is, reads the text
/// in it, or reads and translates it. The answer also goes to the watch.
class LookScreen extends ConsumerStatefulWidget {
  const LookScreen({super.key});

  @override
  ConsumerState<LookScreen> createState() => _LookScreenState();
}

enum _Mode { describe, read, translate }

class _LookScreenState extends ConsumerState<LookScreen> {
  _Mode _mode = _Mode.describe;
  File? _photo;
  String? _answer;
  bool _busy = false;

  String _prompt() {
    final lang = NotifAi.userLanguage();
    switch (_mode) {
      case _Mode.describe:
        return 'You are the eyes of the user. Say what is in this photo in $lang, in 1-3 short sentences: '
            'the main thing first, then anything useful (text, colours, danger, how many). /no_think';
      case _Mode.read:
        return 'Read all the text in this photo exactly as written, line by line. '
            'If there is no text, say so in $lang. /no_think';
      case _Mode.translate:
        return 'Read the text in this photo and translate it into $lang. '
            'Reply with the translation only. If there is no text, say so in $lang. /no_think';
    }
  }

  static const _labels = {_Mode.describe: 'What is it?', _Mode.read: 'Read the text', _Mode.translate: 'Translate it'};

  Future<void> _take(ImageSource src) async {
    if (await GemmaService.instance.visionModel() == null) {
      if (mounted) showSnack(context, 'Download a vision model first (Transcription & AI > Look)');
      return;
    }
    final x = await ImagePicker().pickImage(source: src, maxWidth: 1024, maxHeight: 1024, imageQuality: 85);
    if (x == null) return;
    setState(() {
      _photo = File(x.path);
      _answer = null;
      _busy = true;
    });
    try {
      final a = await GemmaService.instance.look(await _photo!.readAsBytes(), _prompt());
      if (!mounted) return;
      setState(() => _answer = (a == null || a.isEmpty) ? 'Nothing came back - try another photo' : a);
      if (a != null && a.isNotEmpty) await _toWatch(a);
    } catch (e) {
      if (mounted) setState(() => _answer = 'Couldn\'t look at it: ${e.toString().replaceFirst('Exception: ', '')}');
    } finally {
      if (mounted) setState(() => _busy = false);
    }
  }

  // The answer as a notification on the watch (it keeps it in its history).
  Future<void> _toWatch(String text) async {
    final ble = ref.read(bleServiceProvider);
    if (!ble.hasPhoneLink) return;
    await ble.sendLink({
      't': 'ntf',
      'id': 0x4C4F0000 | (DateTime.now().millisecondsSinceEpoch & 0xFFFF),
      'ap': 'Look',
      'ti': _labels[_mode],
      'tx': text.length > 240 ? '${text.substring(0, 237)}...' : text,
      'rp': 0,
    });
  }

  Future<void> _say() async {
    final a = _answer;
    if (a == null) return;
    final tts = TtsService.instance;
    final code = NotifAi.userLanguage() == 'Italian' ? 'it' : 'en';
    final v = await tts.voiceFor(code);
    if (v == null) {
      if (mounted) showSnack(context, 'Install a voice in Transcription & AI to hear it');
      return;
    }
    final f = File('${(await getTemporaryDirectory()).path}/look_say.wav');
    await f.writeAsBytes(await tts.speak(v, a.length > 360 ? a.substring(0, 360) : a));
    await ref.read(memoPlayerProvider.notifier).playPath(f.path);
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(title: const Text('Look')),
      body: ListView(padding: const EdgeInsets.fromLTRB(16, 8, 16, 32), children: [
        Wrap(spacing: 8, children: [
          for (final m in _Mode.values)
            ChoiceChip(label: Text(_labels[m]!), selected: _mode == m, onSelected: (_) => setState(() => _mode = m)),
        ]),
        const SizedBox(height: 14),
        Row(children: [
          Expanded(
            child: FilledButton.icon(
              onPressed: _busy ? null : () => _take(ImageSource.camera),
              icon: const Icon(Icons.photo_camera_rounded, size: 18),
              label: const Text('Take a photo'),
            ),
          ),
          const SizedBox(width: 10),
          OutlinedButton.icon(
            onPressed: _busy ? null : () => _take(ImageSource.gallery),
            icon: const Icon(Icons.photo_library_outlined, size: 18),
            label: const Text('Gallery'),
          ),
        ]),
        if (_photo != null) ...[
          const SizedBox(height: 14),
          ClipRRect(borderRadius: BorderRadius.circular(14), child: Image.file(_photo!, height: 240, fit: BoxFit.cover)),
        ],
        const SizedBox(height: 14),
        if (_busy)
          const Panel(
            child: Row(children: [
              SizedBox(width: 22, height: 22, child: CircularProgressIndicator(strokeWidth: 2)),
              SizedBox(width: 12),
              Text('Looking...', style: TextStyle(color: AppColors.textDim)),
            ]),
          )
        else if (_answer != null)
          Panel(
            child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
              SelectableText(_answer!, style: const TextStyle(color: AppColors.text, height: 1.45)),
              Align(
                alignment: Alignment.centerRight,
                child: TextButton.icon(
                  onPressed: _say,
                  icon: const Icon(Icons.volume_up_rounded, size: 18),
                  label: const Text('Say it'),
                ),
              ),
            ]),
          ),
        const SizedBox(height: 10),
        const Text(
          'Runs on this phone with the vision model from Transcription & AI. The answer also shows on the watch '
          '(Notifications).',
          style: TextStyle(color: AppColors.textFaint, fontSize: 12, height: 1.4),
        ),
      ]),
    );
  }
}
