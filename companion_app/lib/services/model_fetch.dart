import 'dart:async';
import 'dart:io';

import 'package:background_downloader/background_downloader.dart';
import 'package:flutter/foundation.dart';
import 'package:path_provider/path_provider.dart';
import 'package:shared_preferences/shared_preferences.dart';

/// Every AI model download goes through here (Whisper, the language and
/// vision models, voices, speakers, sound alerts, embeddings):
///
///  - the system downloads it (WorkManager on Android, a background
///    URLSession on iOS), so it keeps going with the app in the background
///    or closed; a notification shows the progress;
///  - retries, and a download that stops halfway picks up where it was
///    (allowPause: also across Android's 9-minute background limit);
///  - several addresses per file: Hugging Face files also come from the
///    mirror set in AI models (if any) and hf-mirror.com, tried in turn;
///  - the size is checked before a file counts as downloaded.
///
/// The models' own installers (unpacking, registering) run in the app:
/// [ModelJobs] remembers what was being installed and finishes it at the
/// next start when the app was closed in the meantime.
class ModelFetch {
  ModelFetch._();
  static final ModelFetch instance = ModelFetch._();

  static const group = 'ai_models';
  static const _mirrorKey = 'model_mirror';

  // Our own scope: flutter_gemma uses the default FileDownloader too.
  final FileDownloader _dl = FileDownloader.scoped('amoled');
  final _waiters = <String, Completer<TaskStatusUpdate>>{};
  final _listeners = <String, void Function(double)>{};
  Future<void>? _init;

  Future<void> init() => _init ??= _start();

  Future<void> _start() async {
    _dl.registerCallbacks(group: group, taskStatusCallback: _onStatus, taskProgressCallback: _onProgress);
    _dl.configureNotificationForGroup(
      group,
      running: const TaskNotification('{displayName}', 'Downloading - {progress}'),
      complete: const TaskNotification('{displayName}', 'Downloaded - open AmoledWatch to finish setting it up'),
      error: const TaskNotification('{displayName}', 'Download stopped - open AmoledWatch to try again'),
      paused: const TaskNotification('{displayName}', 'Paused'),
      progressBar: true,
    );
    try {
      await _dl.configure(
        globalConfig: [(Config.requestTimeout, const Duration(seconds: 60))],
        // a notification and a foreground service for files over 40 MB: no 9-minute limit
        androidConfig: [(Config.runInForegroundIfFileLargerThan, 40)],
        iOSConfig: [(Config.resourceTimeout, const Duration(hours: 24))],
      );
    } catch (_) {}
    await _dl.trackTasksInGroup(group);
    await _dl.resumeFromBackground();
    unawaited(
      Future.delayed(
        const Duration(seconds: 3),
        () => _dl.rescheduleKilledTasks(),
      ).catchError((_) => (<Task>[], <Task>[])),
    );
  }

  // ---- mirrors ---------------------------------------------------------------------------
  /// A base URL that serves Hugging Face files under the same paths
  /// (e.g. your own server or a regional mirror), or "".
  Future<String> mirror() async => (await SharedPreferences.getInstance()).getString(_mirrorKey) ?? '';
  Future<void> setMirror(String base) async {
    var b = base.trim();
    while (b.endsWith('/')) {
      b = b.substring(0, b.length - 1);
    }
    await (await SharedPreferences.getInstance()).setString(_mirrorKey, b);
  }

  /// Where to get [url] from, in order. Hugging Face files: your mirror
  /// first, then Hugging Face, then hf-mirror.com.
  Future<List<String>> sources(String url) async {
    const hf = 'https://huggingface.co';
    if (!url.startsWith(hf)) return [url];
    final m = await mirror();
    return [if (m.isNotEmpty) '$m${url.substring(hf.length)}', url, 'https://hf-mirror.com${url.substring(hf.length)}'];
  }

  // ---- downloading ----------------------------------------------------------------------
  /// Downloads [url] to [dest] (or picks up a download of it already under
  /// way - also one started before the app was closed). [sizeBytes]: what
  /// it should be roughly, 0 = don't check. [onProgress] 0..1.
  Future<File> fetch({
    required String label,
    required String url,
    required File dest,
    int sizeBytes = 0,
    Map<String, String> headers = const {},
    void Function(double)? onProgress,
  }) async {
    await init();
    if (await _complete(dest, sizeBytes)) return dest;
    final (base, dir) = await _place(dest.parent);
    await Directory(dest.parent.path).create(recursive: true);
    final name = dest.uri.pathSegments.last;
    final urls = await sources(url);
    Object? last;
    for (var i = 0; i < urls.length; i++) {
      final id = 'm${_hash('$dir/$name')}_$i';
      try {
        final st = await _run(
          id,
          () => DownloadTask(
            taskId: id,
            url: urls[i],
            filename: name,
            directory: dir,
            baseDirectory: base,
            group: group,
            headers: headers,
            updates: Updates.statusAndProgress,
            retries: 5,
            allowPause: true, // resumes instead of starting over
            priority: 0, // user-initiated: no time limit on Android 14+
            displayName: label,
          ),
          onProgress,
        );
        if (st.status == TaskStatus.complete) {
          if (await _complete(dest, sizeBytes)) {
            onProgress?.call(1);
            return dest;
          }
          last = 'the file came out too small';
          if (await dest.exists()) await dest.delete();
          continue;
        }
        if (st.status == TaskStatus.canceled) throw const ModelFetchCanceled();
        final code = st.responseStatusCode;
        last = code == 401 || code == 403
            ? 'not allowed (HTTP $code)${url.contains('huggingface') ? ' - check the Hugging Face token and the model\'s license' : ''}'
            : code == 404
            ? 'not found (HTTP 404)'
            : (st.exception?.description ?? 'HTTP ${code ?? '?'}');
      } on ModelFetchCanceled {
        rethrow;
      } catch (e) {
        last = e;
      }
    }
    throw Exception('Download failed: $last');
  }

  Future<TaskStatusUpdate> _run(String id, DownloadTask Function() make, void Function(double)? onProgress) async {
    final c = Completer<TaskStatusUpdate>();
    _waiters[id] = c;
    if (onProgress != null) _listeners[id] = onProgress;
    try {
      final existing = await _dl.taskForId(id);
      final rec = await _dl.database.recordForId(id);
      if (existing is DownloadTask && rec?.status == TaskStatus.paused) {
        if (!await _dl.resume(existing)) await _dl.enqueue(make());
      } else if (existing == null || rec == null || rec.status.isFinalState) {
        if (!await _dl.enqueue(make())) throw Exception('Couldn\'t start the download');
      }
      // else: it's running already (from before the app was closed) - wait for it
      return await c.future;
    } finally {
      _waiters.remove(id);
      _listeners.remove(id);
    }
  }

  void _onStatus(TaskStatusUpdate u) {
    if (u.status.isFinalState) _waiters[u.task.taskId]?.complete(u);
  }

  void _onProgress(TaskProgressUpdate u) {
    if (u.progress >= 0 && u.progress <= 1) _listeners[u.task.taskId]?.call(u.progress);
  }

  /// Stops [dest]'s download (from any address).
  Future<void> cancel(File dest) async {
    final (_, dir) = await _place(dest.parent);
    final name = dest.uri.pathSegments.last;
    final h = _hash('$dir/$name');
    await _dl.cancelTasksWithIds([for (var i = 0; i < 3; i++) 'm${h}_$i']);
  }

  Future<bool> _complete(File f, int size) async =>
      await f.exists() && (size <= 0 ? await f.length() > 0 : await f.length() >= size * 0.85);

  /// The folder as background_downloader keeps it: relative to an app
  /// folder (iOS moves the app's folders between launches).
  Future<(BaseDirectory, String)> _place(Directory d) async {
    final p = d.absolute.path;
    final bases = <(BaseDirectory, Future<Directory> Function())>[
      (BaseDirectory.applicationSupport, getApplicationSupportDirectory),
      if (Platform.isIOS || Platform.isMacOS) (BaseDirectory.applicationLibrary, getLibraryDirectory),
      (BaseDirectory.applicationDocuments, getApplicationDocumentsDirectory),
      (BaseDirectory.temporary, getTemporaryDirectory),
    ];
    for (final (b, get) in bases) {
      final root = (await get()).absolute.path;
      if (p == root) return (b, '');
      if (p.startsWith('$root/')) return (b, p.substring(root.length + 1));
    }
    return (BaseDirectory.root, p);
  }

  static int _hash(String s) {
    var h = 0x811c9dc5;
    for (final c in s.codeUnits) {
      h = ((h ^ c) * 0x01000193) & 0x7fffffff;
    }
    return h;
  }
}

class ModelFetchCanceled implements Exception {
  const ModelFetchCanceled();
  @override
  String toString() => 'Cancelled';
}

/// What's being installed (downloads + unpacking/registering), shared by
/// every screen: progress survives leaving the screen, and an install
/// interrupted by the app closing is finished at the next start
/// ([resume], called from main).
class ModelJobs extends ChangeNotifier {
  ModelJobs._();
  static final ModelJobs instance = ModelJobs._();

  static const _pendingKey = 'model_jobs_pending';

  /// id -> progress 0..1 (-1 = starting / unpacking)
  final Map<String, double> progress = {};
  final Map<String, String> errors = {};
  final Map<String, Future<void> Function(void Function(double) onProgress)> _runners = {};

  bool busy(String id) => progress.containsKey(id);

  /// Every installer, by id (ai_catalog.dart registers them), so pending
  /// jobs can be resumed after a restart.
  void register(String id, Future<void> Function(void Function(double) onProgress) run) => _runners[id] = run;

  /// Installs [id] (once - a second call while it runs waits for the first).
  Future<void> run(String id, {Future<void> Function(void Function(double))? job}) async {
    if (busy(id)) {
      while (busy(id)) {
        await Future.delayed(const Duration(milliseconds: 400));
      }
      if (errors[id] != null) throw Exception(errors[id]);
      return;
    }
    final go = job ?? _runners[id];
    if (go == null) throw Exception('Unknown model $id');
    errors.remove(id);
    progress[id] = -1;
    notifyListeners();
    await _setPending(id, true);
    var last = 0.0;
    try {
      await go((p) {
        if (p - last >= 0.01 || p >= 1) {
          last = p;
          progress[id] = p;
          notifyListeners();
        }
      });
      await _setPending(id, false);
    } on ModelFetchCanceled {
      await _setPending(id, false);
      rethrow;
    } catch (e) {
      errors[id] = e.toString().replaceFirst('Exception: ', '');
      await _setPending(id, false);
      rethrow;
    } finally {
      progress.remove(id);
      notifyListeners();
    }
  }

  /// Several installs one after another (a pack). All of them are
  /// remembered first, so closing the app halfway finishes the rest at the
  /// next start. Returns the ones that failed.
  Future<List<String>> runAll(List<String> ids) async {
    for (final id in ids) {
      await _setPending(id, true);
    }
    final failed = <String>[];
    for (final id in ids) {
      try {
        await run(id);
      } catch (_) {
        failed.add(id);
      }
    }
    return failed;
  }

  /// At app start: finish what was being installed when the app closed,
  /// one after another.
  Future<void> resume() async {
    final ids = [
      for (final id in await pending())
        if (_runners.containsKey(id)) id,
    ];
    for (final id in ids) {
      try {
        await run(id);
      } catch (_) {}
    }
  }

  Future<List<String>> pending() async =>
      (await SharedPreferences.getInstance()).getStringList(_pendingKey) ?? const <String>[];

  Future<void> _setPending(String id, bool on) async {
    final p = await SharedPreferences.getInstance();
    final l = {...(p.getStringList(_pendingKey) ?? const <String>[])};
    on ? l.add(id) : l.remove(id);
    await p.setStringList(_pendingKey, l.toList());
  }
}
