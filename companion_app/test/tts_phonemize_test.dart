import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:amoled_companion/services/tts_service.dart';

void main() {
  test('the espeak voice comes from the Piper voice name', () {
    expect(espeakVoiceFor('vits-piper-en_US-amy-low'), 'en-us');
    expect(espeakVoiceFor('vits-piper-en_GB-alan-medium'), 'en-gb-x-rp');
    expect(espeakVoiceFor('vits-piper-it_IT-riccardo-x_low'), 'it');
    expect(espeakVoiceFor('vits-piper-pt_BR-faber-medium'), 'pt-br');
    expect(espeakVoiceFor('vits-piper-zh_CN-huayan-medium'), 'cmn');
  });

  test('a voice\'s online sample is in rhasspy/piper-voices', () {
    String? u(String id) => TtsService.sampleUrl(TtsVoice(id, '', 'en', 0));
    const hf = 'https://huggingface.co/rhasspy/piper-voices/resolve/main';
    expect(u('vits-piper-en_US-amy-low'), '$hf/en/en_US/amy/low/samples/speaker_0.mp3');
    expect(u('vits-piper-it_IT-riccardo-x_low'), '$hf/it/it_IT/riccardo/x_low/samples/speaker_0.mp3');
    expect(u('vits-piper-uk_UA-ukrainian_tts-medium'), '$hf/uk/uk_UA/ukrainian_tts/medium/samples/speaker_0.mp3');
    for (final v in TtsService.voices) {
      expect(TtsService.sampleUrl(v), isNotNull, reason: v.id);
    }
  });

  // Needs a folder holding espeak-ng-data (PHONEMIZE_DIR) and the piper_phonemize library on LD_LIBRARY_PATH
  final dir = Platform.environment['PHONEMIZE_DIR'];
  test('text becomes phonemes, one list per sentence', () {
    final p = phonemize('Hello world. How are you?', '$dir/espeak-ng-data', 'en-us');
    expect(p.length, 2);
    expect(String.fromCharCodes(p.first), contains('w'));
    print(p.map(String.fromCharCodes).toList());
  }, skip: dir == null ? 'no PHONEMIZE_DIR' : false);
}
