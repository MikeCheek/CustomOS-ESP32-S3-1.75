import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';
import '../providers/ble_provider.dart';
import '../services/wake_words.dart';
import '../theme/app_theme.dart';
import '../widgets/common.dart';

/// Puts wake words on the watch (Settings > Sound > Voice on the watch
/// turns listening on and picks between the installed ones).
class WakeWordScreen extends ConsumerStatefulWidget {
  const WakeWordScreen({super.key});

  @override
  ConsumerState<WakeWordScreen> createState() => _WakeWordScreenState();
}

class _WakeWordScreenState extends ConsumerState<WakeWordScreen> {
  String? _busy;
  double _progress = 0;
  final Set<String> _sent = {};

  Future<void> _install(WakeWord w) async {
    final ble = ref.read(bleServiceProvider);
    if (!ble.isConnected) {
      showSnack(context, 'Connect the watch first');
      return;
    }
    setState(() { _busy = w.dir; _progress = 0; });
    try {
      await WakeWords.install(ble, w, onProgress: (p) {
        if (mounted) setState(() => _progress = p);
      });
      _sent.add(w.dir);
      if (mounted) showSnack(context, '"${w.word}" is on the watch: Settings > Sound > Voice');
    } catch (e) {
      if (mounted) showSnack(context, 'Couldn\'t install "${w.word}": $e');
    } finally {
      if (mounted) setState(() => _busy = null);
    }
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(title: const Text('Wake word')),
      body: ListView(
        padding: const EdgeInsets.fromLTRB(16, 8, 16, 32),
        children: [
          const Text(
            'Say the word and the watch opens the assistant, no button needed. It runs on the watch '
            '(Espressif WakeNet, both microphones) and needs the SD card. Install one or more, then turn '
            'it on in Settings > Sound > Voice on the watch. Listening all the time uses more battery.',
            style: TextStyle(color: AppColors.textDim, fontSize: 13, height: 1.4),
          ),
          const SectionLabel('Words'),
          for (final w in WakeWords.offered) ...[
            Panel(
              child: Column(
                children: [
                  Row(
                    children: [
                      Icon(_sent.contains(w.dir) ? Icons.check_circle_rounded : Icons.record_voice_over_rounded,
                          color: _sent.contains(w.dir) ? AppColors.success : AppColors.textDim),
                      const SizedBox(width: 14),
                      Expanded(
                        child: Text('"${w.word}"',
                            style: const TextStyle(color: AppColors.text, fontSize: 15, fontWeight: FontWeight.w600)),
                      ),
                      if (_busy == w.dir)
                        const SizedBox(width: 22, height: 22, child: CircularProgressIndicator(strokeWidth: 2))
                      else
                        TextButton(
                          onPressed: _busy == null ? () => _install(w) : null,
                          child: Text(_sent.contains(w.dir) ? 'Send again' : 'Install'),
                        ),
                    ],
                  ),
                  if (_busy == w.dir) ...[
                    const SizedBox(height: 12),
                    ClipRRect(
                      borderRadius: BorderRadius.circular(4),
                      child: LinearProgressIndicator(value: _progress > 0 ? _progress : null, minHeight: 6),
                    ),
                  ],
                ],
              ),
            ),
            const SizedBox(height: 10),
          ],
          const SizedBox(height: 4),
          const Text(
            'Models from github.com/espressif/esp-sr (about 290 KB each, sent over Bluetooth).',
            style: TextStyle(color: AppColors.textFaint, fontSize: 12, height: 1.4),
          ),
        ],
      ),
    );
  }
}
