import 'dart:async';
import 'dart:io';
import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:path_provider/path_provider.dart';
import 'package:permission_handler/permission_handler.dart';
import '../providers/companion_provider.dart';
import '../services/assistant_intents.dart';
import '../services/memo_player.dart';
import '../services/phone_recorder.dart';
import '../services/tts_service.dart';
import '../theme/app_theme.dart';
import '../widgets/common.dart';

/// The voice assistant on the phone: the same as on the watch (rules, the
/// local LLM, reminders, music, weather...), answered here and said out
/// loud with the installed voice.
class AssistantScreen extends ConsumerStatefulWidget {
  const AssistantScreen({super.key});

  @override
  ConsumerState<AssistantScreen> createState() => _AssistantScreenState();
}

class _AssistantScreenState extends ConsumerState<AssistantScreen> {
  static const _maxMs = 10000, _quietMs = 1300;
  bool _listening = false, _thinking = false;
  int _level = 0, _floor = 100, _ms = 0, _lastVoice = 0;
  bool _spoke = false;
  Timer? _poll;
  String _path = '';
  final List<(String, String)> _turns = []; // (heard, answer), newest last

  @override
  void dispose() {
    _poll?.cancel();
    if (_listening) PhoneRecorder.cancel();
    super.dispose();
  }

  Future<void> _listen() async {
    if (!(await Permission.microphone.request()).isGranted) {
      if (mounted) showSnack(context, 'Microphone access is needed');
      return;
    }
    _path = '${(await getTemporaryDirectory()).path}/phone_ask.wav';
    if (!await PhoneRecorder.start(_path)) {
      if (mounted) showSnack(context, (await PhoneRecorder.poll()).error ?? 'Could not start the microphone');
      return;
    }
    setState(() {
      _listening = true;
      _spoke = false;
      _floor = 100;
      _ms = 0;
      _lastVoice = 0;
    });
    // Same voice-activity rule as the watch: learn the noise floor, stop
    // after a pause once something was said.
    _poll = Timer.periodic(const Duration(milliseconds: 100), (_) async {
      final p = await PhoneRecorder.poll();
      if (!mounted) return;
      _ms = p.ms;
      _level = p.level;
      if (_ms < 300) {
        if (_level < _floor) _floor = _level;
      } else if (_level > _floor + 10) {
        _spoke = true;
        _lastVoice = _ms;
      }
      setState(() {});
      if (_ms >= _maxMs || (_spoke && _ms - _lastVoice > _quietMs) || (!_spoke && _ms > 5000)) _stop();
    });
  }

  Future<void> _stop() async {
    _poll?.cancel();
    if (!_listening) return;
    setState(() {
      _listening = false;
      _thinking = true;
    });
    try {
      await PhoneRecorder.stop();
      final r = await ref.read(companionProvider.notifier).askFromPhone(_path);
      setState(() => _turns.add((r.heard, r.answer)));
      unawaited(_say(r.heard, r.answer));
    } catch (e) {
      if (mounted) showSnack(context, e.toString().replaceFirst('Exception: ', ''));
    } finally {
      if (mounted) setState(() => _thinking = false);
      try {
        await File(_path).delete();
      } catch (_) {}
    }
  }

  Future<void> _say(String heard, String answer) async {
    final tts = TtsService.instance;
    if (!await tts.enabled()) return;
    final v = await tts.voiceFor(parseAssistantIntent(heard, DateTime.now()).italian ? 'it' : 'en');
    if (v == null) return;
    final f = File('${(await getTemporaryDirectory()).path}/phone_say.wav');
    await f.writeAsBytes(await tts.speak(v, answer));
    await ref.read(memoPlayerProvider.notifier).playPath(f.path);
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(title: const Text('Assistant')),
      body: Column(children: [
        Expanded(
          child: _turns.isEmpty
              ? const Center(
                  child: Padding(
                    padding: EdgeInsets.all(32),
                    child: Text(
                      'Ask anything: "set a timer for 10 minutes", "remind me to call Luca at 6", '
                      '"what\'s the weather", "what did I say about the trip?" - in English or Italian. '
                      'Everything runs on this phone.',
                      textAlign: TextAlign.center,
                      style: TextStyle(color: AppColors.textDim, height: 1.5),
                    ),
                  ),
                )
              : ListView(padding: const EdgeInsets.all(16), children: [
                  for (final (q, a) in _turns) ...[
                    Align(
                      alignment: Alignment.centerRight,
                      child: Panel(
                        color: AppColors.accent.withValues(alpha: 0.15),
                        child: Text(q, style: const TextStyle(color: AppColors.text)),
                      ),
                    ),
                    const SizedBox(height: 8),
                    Panel(child: Text(a, style: const TextStyle(color: AppColors.text, height: 1.4))),
                    const SizedBox(height: 16),
                  ],
                ]),
        ),
        Padding(
          padding: const EdgeInsets.only(bottom: 28, top: 8),
          child: Column(children: [
            Text(
              _listening ? 'Listening...' : (_thinking ? 'Thinking...' : 'Tap and ask'),
              style: const TextStyle(color: AppColors.textDim),
            ),
            const SizedBox(height: 12),
            GestureDetector(
              onTap: _thinking ? null : (_listening ? _stop : _listen),
              child: AnimatedContainer(
                duration: const Duration(milliseconds: 100),
                width: 84 + (_listening ? _level * 0.3 : 0),
                height: 84 + (_listening ? _level * 0.3 : 0),
                decoration: BoxDecoration(
                  shape: BoxShape.circle,
                  color: _listening ? AppColors.accent3 : AppColors.accent,
                ),
                child: _thinking
                    ? const Padding(padding: EdgeInsets.all(28), child: CircularProgressIndicator(strokeWidth: 3))
                    : Icon(_listening ? Icons.stop_rounded : Icons.mic_rounded, color: Colors.white, size: 38),
              ),
            ),
          ]),
        ),
      ]),
    );
  }
}
