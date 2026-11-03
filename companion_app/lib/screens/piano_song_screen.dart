import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'dart:typed_data';
import 'package:flutter/foundation.dart' show compute;
import 'package:file_picker/file_picker.dart';
import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:path_provider/path_provider.dart';
import 'package:permission_handler/permission_handler.dart';
import '../providers/ble_provider.dart';
import '../services/phone_recorder.dart';
import '../services/piano_song.dart';
import '../theme/app_theme.dart';
import '../widgets/common.dart';

/// Make a Piano Tiles level: hum (or whistle, sing "la") a tune into the
/// phone, or pick a MIDI file; the melody goes to the watch's /piano/.
class PianoSongScreen extends ConsumerStatefulWidget {
  const PianoSongScreen({super.key});

  @override
  ConsumerState<PianoSongScreen> createState() => _PianoSongScreenState();
}

class _PianoSongScreenState extends ConsumerState<PianoSongScreen> {
  static const _maxMs = 30000;
  final _title = TextEditingController();
  bool _recording = false, _working = false, _sending = false;
  int _ms = 0, _level = 0;
  Timer? _poll;
  String? _wavPath;
  GridMelody? _melody;
  int _bpm = 100;

  @override
  void dispose() {
    _poll?.cancel();
    if (_recording) PhoneRecorder.cancel();
    _title.dispose();
    super.dispose();
  }

  Future<void> _startHum() async {
    final perm = await Permission.microphone.request();
    if (!perm.isGranted) {
      if (mounted) showSnack(context, 'Microphone access is needed to hear the tune');
      return;
    }
    _wavPath = '${(await getTemporaryDirectory()).path}/hum.wav';
    if (!await PhoneRecorder.start(_wavPath!)) {
      final p = await PhoneRecorder.poll();
      if (mounted) showSnack(context, p.error ?? 'Could not start the microphone');
      return;
    }
    setState(() {
      _recording = true;
      _melody = null;
      _ms = 0;
    });
    _poll = Timer.periodic(const Duration(milliseconds: 100), (_) async {
      final p = await PhoneRecorder.poll();
      if (!mounted) return;
      setState(() {
        _ms = p.ms;
        _level = p.level;
      });
      if (p.ms >= _maxMs) _stopHum();
    });
  }

  Future<void> _stopHum() async {
    _poll?.cancel();
    if (!_recording) return;
    setState(() {
      _recording = false;
      _working = true;
    });
    try {
      await PhoneRecorder.stop();
      // Pitch tracking is a few seconds of maths: off the UI thread.
      final m = await compute(_analyzeHum, _wavPath!);
      if (m.length < 3) throw Exception('Couldn\'t hear a tune - hum a little louder, one note at a time');
      _setMelody(m, 'Hummed tune');
    } catch (e) {
      if (mounted) showSnack(context, e.toString().replaceFirst('Exception: ', ''));
    } finally {
      if (mounted) setState(() => _working = false);
    }
  }

  Future<void> _pickMidi() async {
    final r = await FilePicker.platform.pickFiles(type: FileType.any);
    final path = r?.files.single.path;
    if (path == null) return;
    try {
      final m = midiToGrid(await File(path).readAsBytes());
      final name = r!.files.single.name.replaceAll(RegExp(r'\.(mid|midi)$', caseSensitive: false), '');
      _setMelody(m, name.replaceAll('_', ' '));
    } on FormatException catch (e) {
      if (mounted) showSnack(context, e.message);
    }
  }

  void _setMelody(GridMelody m, String title) {
    setState(() {
      _melody = m;
      _bpm = m.bpm;
      if (_title.text.trim().isEmpty) _title.text = title;
    });
  }

  String get _text => pianoSongText(GridMelody(_bpm, _melody!.onsets, _melody!.notes), title: _title.text);

  Future<void> _send() async {
    final ble = ref.read(bleServiceProvider);
    if (!ble.isConnected) {
      showSnack(context, 'Connect the watch first');
      return;
    }
    setState(() => _sending = true);
    try {
      final bytes = Uint8List.fromList(utf8.encode(_text));
      await ble.sendFile('pt:${pianoFileName(_title.text)}', bytes);
      if (mounted) showSnack(context, '"${_title.text.trim()}" is in Piano Tiles on the watch (SD card songs)');
    } catch (e) {
      if (mounted) showSnack(context, 'Couldn\'t send it: $e');
    } finally {
      if (mounted) setState(() => _sending = false);
    }
  }

  @override
  Widget build(BuildContext context) {
    final m = _melody;
    return Scaffold(
      appBar: AppBar(title: const Text('Make a Piano Tiles song')),
      body: ListView(
        padding: const EdgeInsets.fromLTRB(16, 8, 16, 32),
        children: [
          const Text(
            'Hum, whistle or sing "la" a tune - one note at a time - and it becomes a level on the watch. '
            'Or pick a MIDI file and its melody is used.',
            style: TextStyle(color: AppColors.textDim, fontSize: 13, height: 1.4),
          ),
          const SizedBox(height: 16),
          Panel(
            child: Column(
              children: [
                Icon(_recording ? Icons.graphic_eq_rounded : Icons.music_note_rounded,
                    size: 44, color: _recording ? AppColors.accent3 : AppColors.textDim),
                const SizedBox(height: 10),
                if (_recording) ...[
                  ClipRRect(
                    borderRadius: BorderRadius.circular(4),
                    child: LinearProgressIndicator(value: _level / 100, minHeight: 6),
                  ),
                  const SizedBox(height: 8),
                  Text('${(_ms / 1000).toStringAsFixed(1)} s of ${_maxMs ~/ 1000}',
                      style: const TextStyle(color: AppColors.textDim, fontSize: 12)),
                  const SizedBox(height: 10),
                ],
                if (_working)
                  const Padding(
                    padding: EdgeInsets.all(8),
                    child: Text('Listening for the notes...', style: TextStyle(color: AppColors.textDim)),
                  ),
                Row(
                  mainAxisAlignment: MainAxisAlignment.center,
                  children: [
                    FilledButton.icon(
                      onPressed: _working ? null : (_recording ? _stopHum : _startHum),
                      icon: Icon(_recording ? Icons.stop_rounded : Icons.mic_rounded, size: 18),
                      label: Text(_recording ? 'Done' : 'Hum a tune'),
                    ),
                    const SizedBox(width: 10),
                    OutlinedButton.icon(
                      onPressed: _recording || _working ? null : _pickMidi,
                      icon: const Icon(Icons.piano, size: 18),
                      label: const Text('MIDI file'),
                    ),
                  ],
                ),
              ],
            ),
          ),
          if (m != null) ...[
            const SectionLabel('Song'),
            Panel(
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  TextField(controller: _title, decoration: const InputDecoration(labelText: 'Title')),
                  const SizedBox(height: 12),
                  Row(children: [
                    Text('${m.length} notes', style: const TextStyle(color: AppColors.text)),
                    const Spacer(),
                    IconButton(
                      onPressed: _bpm > 40 ? () => setState(() => _bpm -= 5) : null,
                      icon: const Icon(Icons.remove_rounded),
                    ),
                    Text('$_bpm bpm', style: const TextStyle(color: AppColors.text)),
                    IconButton(
                      onPressed: _bpm < 240 ? () => setState(() => _bpm += 5) : null,
                      icon: const Icon(Icons.add_rounded),
                    ),
                  ]),
                  const SizedBox(height: 6),
                  Text(
                    _text.split('\n').skip(_text.contains('composer:') ? 3 : 2).take(3).join('\n'),
                    style: const TextStyle(color: AppColors.textDim, fontFamily: 'monospace', fontSize: 12, height: 1.5),
                    maxLines: 3,
                    overflow: TextOverflow.ellipsis,
                  ),
                  const SizedBox(height: 12),
                  SizedBox(
                    width: double.infinity,
                    child: FilledButton.icon(
                      onPressed: _sending ? null : _send,
                      icon: const Icon(Icons.send_rounded, size: 18),
                      label: Text(_sending ? 'Sending...' : 'Send to the watch'),
                    ),
                  ),
                ],
              ),
            ),
          ],
        ],
      ),
    );
  }
}

// Top level, for compute(): runs in a background isolate.
GridMelody _analyzeHum(String path) {
  final (x, rate) = readWavMono(File(path).readAsBytesSync());
  return gridFromHeard(humToNotes(x, rate));
}
