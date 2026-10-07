import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'app.dart';
import 'providers/ble_provider.dart';
import 'providers/companion_provider.dart';
import 'providers/notes_provider.dart';
import 'services/gemma_service.dart';

/// Runs once per app process. The process can start without any UI (the
/// system binds the notification listener, or the link service restarts),
/// so the watch connection and phone link are started here - not from a
/// widget - and keep running while no screen is shown.
Future<void> main() async {
  WidgetsFlutterBinding.ensureInitialized();

  try {
    await GemmaService.initializeEngine();
  } catch (e) {
    debugPrint('[main] LLM engine init failed: $e');
  }

  final container = ProviderContainer();
  container.read(bleProvider);       // auto-reconnect to the last watch
  container.read(companionProvider); // notifications, calls, media, battery, weather
  container.read(notesProvider);     // recordings sync

  runApp(UncontrolledProviderScope(container: container, child: const AmoledCompanionApp()));
}
