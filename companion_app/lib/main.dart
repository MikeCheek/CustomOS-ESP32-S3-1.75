import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'app.dart';
import 'providers/ble_provider.dart';
import 'providers/companion_provider.dart';
import 'providers/notes_provider.dart';
import 'services/ai_catalog.dart';
import 'services/gemma_service.dart';
import 'services/model_fetch.dart';
import 'services/ai_profiler.dart';
import 'emulator/emulator_controller.dart';

/// Runs once per app process. The process can start without any UI (the
/// system binds the notification listener, or the link service restarts),
/// so the watch connection and phone link are started here - not from a
/// widget - and keep running while no screen is shown.
Future<void> main() async {
  WidgetsFlutterBinding.ensureInitialized();

  // a model run that never finished last time (a crash) becomes a crash entry
  AiProfiler.instance.init().catchError((Object e) => debugPrint('[main] AI profiler: $e'));

  try {
    await GemmaService.initializeEngine();
  } catch (e) {
    debugPrint('[main] LLM engine init failed: $e');
  }
  // the language model is kept loaded between requests: it goes when the phone is short of memory
  WidgetsBinding.instance.addObserver(_MemoryWatch());

  final container = ProviderContainer();
  container.read(bleProvider); // auto-reconnect to the last watch
  container.read(companionProvider); // notifications, calls, media, battery, weather
  container.read(notesProvider); // recordings sync
  // the in-app watch emulator, when it's kept as the watch (Watch emulator screen)
  container.read(emulatorProvider).autoStart().catchError((Object e) {
    debugPrint('[main] emulator: $e');
  });

  // AI model downloads: they run in the background and outlast the app;
  // finish setting up the ones that completed (or were cut short) meanwhile.
  AiCatalog.registerJobs();
  ModelFetch.instance.init().then((_) => ModelJobs.instance.resume()).catchError((Object e) {
    debugPrint('[main] model downloads: $e');
  });

  runApp(UncontrolledProviderScope(container: container, child: const AmoledCompanionApp()));
}

class _MemoryWatch with WidgetsBindingObserver {
  @override
  void didHaveMemoryPressure() => GemmaService.instance.onMemoryPressure();
}
