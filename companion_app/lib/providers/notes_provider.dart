import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'dart:typed_data';
import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:path_provider/path_provider.dart';
import 'package:shared_preferences/shared_preferences.dart';
import '../services/ble_service.dart';
import '../services/ble_protocol.dart';
import '../services/notes_storage.dart';
import '../services/whisper_service.dart';
import '../services/gemma_service.dart';
import '../services/wifi_transfer.dart';
import '../services/text_insights.dart';
import 'ble_provider.dart';
import 'settings_provider.dart';
import 'knowledge_provider.dart';

class NotesState {
  final List<RecordingEntry> recordings;
  final bool isLoading;
  final bool isSyncing;
  final String? error;
  final int? downloadingIdx;
  final int totalToDownload;
  final int downloadedCount;
  final double downloadProgress;
  final String? downloadingName;
  final bool processingTranscript;
  final String? processingName;     // recording being transcribed/summarized
  final String processingStage;     // human readable step
  final double processingProgress;  // 0..1, or <0 = indeterminate
  final Set<String> downloadedFiles;
  final String? transferVia;        // "Wi-Fi" / "Bluetooth" while downloading
  final double transferRate;        // KB/s of the current transfer
  final String? uploadingName;      // adding a recording to the watch
  final double uploadProgress;
  // One-tap "process everything" (sync, download, transcribe, summarize)
  final bool pipelineRunning;
  final int pipelineStep;           // 0 sync, 1 download, 2 transcribe, 3 summarize, 4 insights
  final String pipelineStage;       // human readable
  final int pipelineDone, pipelineTotal;
  final String? pipelineReport;     // what the last run did

  NotesState({
    this.recordings = const [],
    this.isLoading = false,
    this.isSyncing = false,
    this.error,
    this.downloadingIdx,
    this.totalToDownload = 0,
    this.downloadedCount = 0,
    this.downloadProgress = 0,
    this.downloadingName,
    this.processingTranscript = false,
    this.processingName,
    this.processingStage = '',
    this.processingProgress = -1,
    this.downloadedFiles = const {},
    this.transferVia,
    this.transferRate = 0,
    this.uploadingName,
    this.uploadProgress = 0,
    this.pipelineRunning = false,
    this.pipelineStep = 0,
    this.pipelineStage = '',
    this.pipelineDone = 0,
    this.pipelineTotal = 0,
    this.pipelineReport,
  });

  // error / downloadingIdx / downloadingName are nullable on purpose:
  // leave them out to keep the current value, pass null to clear them.
  // (They used to be cleared by EVERY copyWith that didn't mention them,
  // so e.g. the first progress update hid the download banner.)
  static const _keep = Object();

  NotesState copyWith({
    List<RecordingEntry>? recordings,
    bool? isLoading,
    bool? isSyncing,
    Object? error = _keep,
    Object? downloadingIdx = _keep,
    int? totalToDownload,
    int? downloadedCount,
    double? downloadProgress,
    Object? downloadingName = _keep,
    bool? processingTranscript,
    Object? processingName = _keep,
    String? processingStage,
    double? processingProgress,
    Set<String>? downloadedFiles,
    Object? transferVia = _keep,
    double? transferRate,
    Object? uploadingName = _keep,
    double? uploadProgress,
    bool? pipelineRunning,
    int? pipelineStep,
    String? pipelineStage,
    int? pipelineDone,
    int? pipelineTotal,
    Object? pipelineReport = _keep,
  }) {
    return NotesState(
      recordings: recordings ?? this.recordings,
      isLoading: isLoading ?? this.isLoading,
      isSyncing: isSyncing ?? this.isSyncing,
      error: identical(error, _keep) ? this.error : error as String?,
      downloadingIdx: identical(downloadingIdx, _keep) ? this.downloadingIdx : downloadingIdx as int?,
      totalToDownload: totalToDownload ?? this.totalToDownload,
      downloadedCount: downloadedCount ?? this.downloadedCount,
      downloadProgress: downloadProgress ?? this.downloadProgress,
      downloadingName: identical(downloadingName, _keep) ? this.downloadingName : downloadingName as String?,
      processingTranscript: processingTranscript ?? this.processingTranscript,
      processingName: identical(processingName, _keep) ? this.processingName : processingName as String?,
      processingStage: processingStage ?? this.processingStage,
      processingProgress: processingProgress ?? this.processingProgress,
      downloadedFiles: downloadedFiles ?? this.downloadedFiles,
      transferVia: identical(transferVia, _keep) ? this.transferVia : transferVia as String?,
      transferRate: transferRate ?? this.transferRate,
      uploadingName: identical(uploadingName, _keep) ? this.uploadingName : uploadingName as String?,
      uploadProgress: uploadProgress ?? this.uploadProgress,
      pipelineRunning: pipelineRunning ?? this.pipelineRunning,
      pipelineStep: pipelineStep ?? this.pipelineStep,
      pipelineStage: pipelineStage ?? this.pipelineStage,
      pipelineDone: pipelineDone ?? this.pipelineDone,
      pipelineTotal: pipelineTotal ?? this.pipelineTotal,
      pipelineReport: identical(pipelineReport, _keep) ? this.pipelineReport : pipelineReport as String?,
    );
  }
}

class NotesNotifier extends StateNotifier<NotesState> {
  final BleService _ble;
  StreamSubscription? _notesSub;
  final List<int> _downloadBuffer = [];
  String _downloadFilename = '';
  Completer<void>? _downloadCompleter;
  int _syncSessionId = 0;
  // A transient download (voice reply dictation): bytes go to this
  // completer instead of the recordings folder / memo list.
  String? _tempName;
  Completer<Uint8List>? _tempCompleter;

  /// Wi-Fi transfers allowed (settings) - read when a transfer starts.
  final bool Function() _wifiAllowed;
  /// "Process new memos after a sync" (settings).
  final bool Function() _autoProcess;
  /// Run at the end of the pipeline (names the theme collections).
  Future<void> Function()? afterPipeline;
  bool _pipelineCancel = false;
  Completer<void>? _listWait;       // the pipeline waits for the file list
  // One Wi-Fi session shared by everything that needs it; closed (the
  // watch turns Wi-Fi off) when the last user is done.
  Future<WifiSession>? _wifiOpen;
  int _wifiUsers = 0;
  DateTime _wifiBackoffUntil = DateTime.fromMillisecondsSinceEpoch(0);
  bool _batchRunning = false;     // an auto-download batch owns the transfers
  int _prioUsers = 0;             // high BLE connection priority, ref-counted
  // Windowed Bluetooth downloads (firmware 2.7+), decided per download.
  static const _window = 16, _creditStep = 8;
  int _sinceCredit = 0;
  bool _windowed = false;
  DateTime _xferStart = DateTime.now();
  int _xferBytes = 0;

  NotesNotifier(this._ble, this._wifiAllowed, this._autoProcess) : super(NotesState()) {
    _loadLocal();
    _listenBleNotes();
    _initServices();
  }

  Future<void> _loadLocal() async {
    state = state.copyWith(isLoading: true);
    final entries = await NotesStorage.loadAll();
    state = state.copyWith(recordings: entries, isLoading: false);
  }

  Future<void> _initServices() async {
    await WhisperService.instance.loadPrefs();
  }

  void _listenBleNotes() {
    _notesSub?.cancel();
    _notesSub = _ble.notesStream.listen((event) async {
      final type = event['type'] as String?;

      if (type == 'fileList') {
        final jsonStr = event['data'] as String;
        await _handleFileList(jsonStr);
      } else if (type == 'fileChunk') {
        final data = event['data'];
        final chunkIdx = event['chunkIdx'] as int;
        final totalChunks = event['totalChunks'] as int;
        _handleFileChunk(data, chunkIdx, totalChunks);
      } else if (type == 'transferComplete') {
        await _handleTransferComplete();
      }
    });
  }

  Future<void> _handleFileList(String jsonStr) async {
    try {
      print('[Notes] Received file list: ${jsonStr.length} chars');
      final list = jsonDecode(jsonStr) as List<dynamic>;
      print('[Notes] Parsed ${list.length} files from watch');

      final dir = await getApplicationDocumentsDirectory();
      final recordingsDir = Directory('${dir.path}/recordings');

      final watchFiles = <RecordingEntry>[];
      for (final e in list) {
        final name = e['name'] as String? ?? '';
        final size = e['size'] as int? ?? 0;
        if (name.isEmpty || size <= 0) {
          print('[Notes] Skipping file: name="$name" size=$size');
          continue;
        }
        watchFiles.add(RecordingEntry(name: name, size: size));
      }

      final local = await NotesStorage.loadAll();
      final merged = <RecordingEntry>[];

      for (final wf in watchFiles) {
        final existing = local.where((l) => l.name == wf.name).firstOrNull;
        if (existing != null) {
          merged.add(existing.copyWith(size: wf.size)); // keeps transcript, title, topics...
        } else {
          merged.add(wf);
        }
      }

      for (final l in local) {
        if (!merged.any((m) => m.name == l.name)) {
          merged.add(l);
        }
      }

      await NotesStorage.saveAll(merged);

      // Check which files exist locally
      final downloaded = <String>{};
      if (await recordingsDir.exists()) {
        for (final entry in merged) {
          final localFile = File('${recordingsDir.path}/${entry.name}');
          if (await localFile.exists()) {
            final localSize = await localFile.length();
            if (localSize == entry.size) {
              downloaded.add(entry.name);
            }
          }
        }
      }

      // Show recordings IMMEDIATELY — no endless loading
      state = state.copyWith(
        recordings: merged,
        isSyncing: false,
        error: null,
        downloadedFiles: downloaded,
      );

      // Auto-download missing files in background
      _autoDownloadMissing(watchFiles);
      // Phone memos the watch doesn't have yet
      Future(() => _flushPending(onWatch: watchFiles.map((w) => w.name).toSet()));

      final waiter = _listWait;
      _listWait = null;
      if (waiter != null) {
        if (!waiter.isCompleted) waiter.complete();
      } else if (_autoProcess() && !state.pipelineRunning &&
          watchFiles.any((w) => !local.any((l) => l.name == w.name))) {
        // New memos on the watch (not seen before): download, transcribe and
        // summarize them. Older failures wait for a manual run.
        Future(() => runPipeline(skipSync: true, quiet: true));
      }
    } catch (e) {
      state = state.copyWith(error: 'Failed to parse file list: $e', isSyncing: false);
      final waiter = _listWait;
      _listWait = null;
      if (waiter != null && !waiter.isCompleted) waiter.complete();
    }
  }

  Future<void> _autoDownloadMissing(List<RecordingEntry> watchFiles) async {
    if (!_ble.isConnected || _batchRunning) return;
    _batchRunning = true;
    final session = _syncSessionId;
    try {
      await _autoDownloadMissingInner(watchFiles);
    } finally {
      _batchRunning = false;
      // Stopped early (pipeline cancelled bumped the session after the batch
      // took its own): the inner loop returned without clearing the banner.
      if (mounted && _syncSessionId > session + 1 && state.totalToDownload > 0) {
        state = state.copyWith(
          downloadingIdx: null,
          downloadingName: null,
          downloadProgress: 0,
          totalToDownload: 0,
          downloadedCount: 0,
          transferVia: null,
        );
      }
    }
  }

  Future<void> _autoDownloadMissingInner(List<RecordingEntry> watchFiles) async {
    try {
      final dir = await getApplicationDocumentsDirectory();
      final recordingsDir = Directory('${dir.path}/recordings');
      if (!await recordingsDir.exists()) {
        await recordingsDir.create(recursive: true);
      }

      // Find files that need downloading
      final toDownload = <MapEntry<int, RecordingEntry>>[];
      for (int i = 0; i < watchFiles.length; i++) {
        final wf = watchFiles[i];
        if (wf.name.isEmpty) continue;
        final localFile = File('${recordingsDir.path}/${wf.name}');
        if (await localFile.exists()) {
          final localSize = await localFile.length();
          if (localSize == wf.size) continue;
        }
        toDownload.add(MapEntry(i, wf));
      }

      if (toDownload.isEmpty) return;

      final sessionId = ++_syncSessionId;
      state = state.copyWith(
        totalToDownload: toDownload.length,
        downloadedCount: 0,
      );

      // Big batches go over Wi-Fi when the watch can (20-50x faster);
      // whatever is left (or everything, if Wi-Fi fails) over Bluetooth.
      final totalBytes = toDownload.fold<int>(0, (a, e) => a + e.value.size);
      if (_useWifi(totalBytes, 256 * 1024)) {
        final done = await _downloadViaWifi(toDownload, recordingsDir, () => sessionId != _syncSessionId,
            background: true);
        toDownload.removeWhere((e) => done.contains(e.value.name));
        if (sessionId != _syncSessionId) return;
      }
      await _prio(true);
      try {
        for (final entry in toDownload) {
          if (sessionId != _syncSessionId) return; // New sync started, abort
          if (!_ble.isConnected) break;

          final wf = entry.value;
          final localFile = File('${recordingsDir.path}/${wf.name}');
          if (await localFile.exists()) {
            final localSize = await localFile.length();
            if (localSize == wf.size) continue;
          }

          // A voice reply or a single download has the slot: wait for it (no
          // await between this check and claiming the slot below).
          while (_tempName != null || _downloadFilename.isNotEmpty) {
            await Future.delayed(const Duration(milliseconds: 250));
          }
          if (sessionId != _syncSessionId || !_ble.isConnected) return;
          print('[Notes] Auto-downloading: ${wf.name}');
          final completer = _claim(wf.name);
          state = state.copyWith(
            downloadingIdx: entry.key,
            downloadingName: wf.name,
            downloadProgress: 0,
          );

          try {
            // The watch pushes chunk 0 by itself (see downloadFile below for
            // why an extra requestNextChunk() here was a race).
            await _ble.requestFileDownload(wf.name);
            await _openWindow();
          } catch (e) {
            print('[Notes] Failed to start download for ${wf.name}: $e');
            _downloadBuffer.clear();
            _downloadFilename = '';
            _downloadCompleter = null;
            state = state.copyWith(error: 'Download failed: $e');
            continue;
          }

          try {
            await completer.future.timeout(
              const Duration(seconds: 300),
              onTimeout: () {
                print('[Notes] Download timeout for ${wf.name}');
                if (identical(_downloadCompleter, completer)) {
                  _downloadBuffer.clear();
                  _downloadFilename = '';
                  _downloadCompleter = null;
                }
              },
            );
          } catch (_) {}

          state = state.copyWith(
            downloadedCount: (state.downloadedCount) + 1,
            downloadProgress: 0,
          );
        }
      } finally {
        await _prio(false);
      }

      if (sessionId == _syncSessionId) {
        state = state.copyWith(
          downloadingIdx: null,
          downloadingName: null,
          downloadProgress: 0,
          totalToDownload: 0,
          downloadedCount: 0,
          transferVia: null,
        );
      }
    } catch (e, st) {
      print('[Notes] Auto-download error: $e\n$st');
      state = state.copyWith(
        error: 'Auto-download failed: $e',
        downloadingIdx: null,
        downloadingName: null,
        downloadProgress: 0,
        totalToDownload: 0,
        downloadedCount: 0,
      );
    }
  }

  bool _downloadCompleted = false;
  double _lastReportedProgress = -1;
  int _lastReceivedChunkIdx = -1;

  void _handleFileChunk(dynamic data, int chunkIdx, int totalChunks) {
    if (_downloadFilename.isEmpty || _downloadCompleted) return;

    // Duplicate chunk — flutter_blue_plus delivers each BLE notification
    // twice via onCharacteristicChanged. The first callback processes the
    // data and sends requestNextChunk(); the second hits this guard and
    // returns silently. This is expected and not an error.
    if (chunkIdx <= _lastReceivedChunkIdx) {
      return;
    }
    if (chunkIdx != _lastReceivedChunkIdx + 1) {
      // A notification got lost: the file would be silently corrupt.
      print('[Notes] chunk gap ($_lastReceivedChunkIdx -> $chunkIdx), aborting $_downloadFilename');
      _abortDownload('Download of $_downloadFilename was interrupted - sync again to retry');
      return;
    }
    _lastReceivedChunkIdx = chunkIdx;

    // Log progress at key milestones
    if (chunkIdx == 0 || chunkIdx % 100 == 0 || chunkIdx + 1 >= totalChunks) {
      final pct = totalChunks > 0 ? ((chunkIdx + 1) * 100 ~/ totalChunks) : 0;
      print('[Notes] download $chunkIdx/$totalChunks ($pct%)');
    }

    _downloadBuffer.addAll(List<int>.from(data as List));
    _xferBytes += (data as List).length;
    // A voice reply in the background mustn't drive the memos banner.
    final temp = _tempName != null && _tempName == _downloadFilename;

    // Auto-complete when all chunks received (fallback if 0x13 lost)
    if (totalChunks > 0 && chunkIdx + 1 >= totalChunks) {
      print('[Notes] All $totalChunks chunks received, auto-completing download');
      _downloadCompleted = true;
      if (!temp) state = state.copyWith(downloadProgress: 1.0);
      _handleTransferComplete();
      return;
    }

    // Throttle UI updates to max once per 1% progress
    final progress = totalChunks > 0 ? (chunkIdx + 1) / totalChunks : 0.0;
    if (!temp && (progress - _lastReportedProgress >= 0.01 || progress >= 1.0)) {
      _lastReportedProgress = progress;
      state = state.copyWith(downloadProgress: progress, transferRate: _rate(), transferVia: 'Bluetooth');
    }

    // Flow control. Firmware 2.7+: top the window up every few chunks so
    // several are always in flight; older firmware: one request per chunk.
    if (_windowed) {
      if (++_sinceCredit >= _creditStep) {
        _sinceCredit = 0;
        _ble.requestNextChunk(_creditStep);
      }
    } else {
      _ble.requestNextChunk();
    }
  }

  /// Takes the (single) Bluetooth download slot for [name]. No awaits:
  /// callers check the slot is free right before.
  Completer<void> _claim(String name) {
    _downloadBuffer.clear();
    _downloadFilename = name;
    _downloadCompleted = false;
    _lastReportedProgress = -1;
    _lastReceivedChunkIdx = -1;
    _sinceCredit = 0;
    _windowed = _ble.fastTransfers;   // fixed for this download
    _xferStart = DateTime.now();
    _xferBytes = 0;
    return _downloadCompleter = Completer<void>();
  }

  /// Opens the window of chunks in flight for a download just requested.
  Future<void> _openWindow() async {
    if (_windowed) await _ble.requestNextChunk(_window);
  }

  /// Runs [fn] with the shared Wi-Fi session (opened if needed, closed
  /// when nobody uses it). Used by the Files screen too.
  Future<T> withWifi<T>(Future<T> Function(WifiSession s) fn) async {
    final s = await _acquireWifi();
    try {
      return await fn(s);
    } finally {
      await _releaseWifi();
    }
  }

  /// Wi-Fi is worth trying now (allowed, supported, not backing off).
  bool wifiUsable({bool files = false}) =>
      _wifiAllowed() &&
      (files ? _ble.wifiFiles : _ble.fastTransfers) &&
      DateTime.now().isAfter(_wifiBackoffUntil);

  Future<void> _prio(bool on) async {
    if (on) {
      if (_prioUsers++ == 0) await _ble.setTransferPriority(true);
    } else if (_prioUsers > 0 && --_prioUsers == 0) {
      await _ble.setTransferPriority(false);
    }
  }

  Future<WifiSession> _acquireWifi() async {
    _wifiUsers++;
    try {
      _wifiOpen ??= WifiSession.open(_ble);
      return await _wifiOpen!;
    } catch (_) {
      _wifiUsers--;
      _wifiOpen = null;
      // Don't retry (and wait half a minute) on every sync.
      _wifiBackoffUntil = DateTime.now().add(const Duration(minutes: 10));
      rethrow;
    }
  }

  Future<void> _releaseWifi() async {
    if (_wifiUsers > 0) _wifiUsers--;
    if (_wifiUsers > 0) return;
    final f = _wifiOpen;
    _wifiOpen = null;
    if (f == null) return;
    try {
      await (await f).close();
    } catch (_) {}
  }

  double _rate() {
    final s = DateTime.now().difference(_xferStart).inMilliseconds / 1000.0;
    return s > 0.2 ? _xferBytes / 1024 / s : 0;
  }

  bool _useWifi(int bytes, int threshold) =>
      _wifiAllowed() && _ble.fastTransfers && bytes > threshold && DateTime.now().isAfter(_wifiBackoffUntil);

  /// Downloads [items] over Wi-Fi. Returns the names that made it.
  /// [background] = an automatic sync: failures are logged, not shown.
  Future<Set<String>> _downloadViaWifi(
      List<MapEntry<int, RecordingEntry>> items, Directory dir, bool Function() cancelled,
      {bool background = false}) async {
    final done = <String>{};
    var acquired = false;
    try {
      state = state.copyWith(
        downloadingIdx: items.first.key,
        downloadingName: items.first.value.name,
        downloadProgress: 0,
        transferVia: 'Wi-Fi (connecting)',
        transferRate: 0,
      );
      final s = await _acquireWifi();
      acquired = true;
      for (final entry in items) {
        if (cancelled()) break;
        final wf = entry.value;
        state = state.copyWith(downloadingIdx: entry.key, downloadingName: wf.name, downloadProgress: 0, transferVia: 'Wi-Fi');
        final t0 = DateTime.now();
        var last = -1.0;
        await s.download(wf.name, File('${dir.path}/${wf.name}'), expected: wf.size, onProgress: (got, total) {
          final p = total > 0 ? got / total : 0.0;
          if (p - last < 0.01 && p < 1) return;
          last = p;
          final secs = DateTime.now().difference(t0).inMilliseconds / 1000.0;
          if (mounted) state = state.copyWith(downloadProgress: p, transferRate: secs > 0.2 ? got / 1024 / secs : 0);
        });
        await _markDownloaded(wf.name);
        done.add(wf.name);
        if (mounted) state = state.copyWith(downloadedCount: state.downloadedCount + 1);
      }
    } catch (e) {
      print('[Notes] Wi-Fi transfer: $e');
      if (mounted) {
        state = state.copyWith(
          transferVia: 'Bluetooth',
          error: background ? state.error : '${e.toString().replaceFirst('Exception: ', '')} - using Bluetooth',
        );
      }
    } finally {
      if (acquired) await _releaseWifi();
    }
    return done;
  }

  /// A recording now complete on the phone: date it, mark it downloaded.
  Future<void> _markDownloaded(String filename) async {
    final entries = await NotesStorage.loadAll();
    final idx = entries.indexWhere((e) => e.name == filename);
    if (idx >= 0 && entries[idx].recordedAt == null) {
      entries[idx].recordedAt = DateTime.now();
      await NotesStorage.save(entries[idx]);
    }
    if (mounted) {
      final updated = Set<String>.from(state.downloadedFiles)..add(filename);
      state = state.copyWith(downloadedFiles: updated);
    }
  }

  void _abortDownload(String message) {
    final wasTemp = _tempName != null && _tempName == _downloadFilename;
    _downloadBuffer.clear();
    _downloadFilename = '';
    _downloadCompleted = true;
    if (wasTemp) {
      final c = _tempCompleter;
      _tempName = null;
      _tempCompleter = null;
      if (c != null && !c.isCompleted) c.completeError(Exception('Transfer interrupted'));
      _downloadCompleter?.complete();
      _downloadCompleter = null;
      return;
    }
    state = state.copyWith(error: message);
    _downloadCompleter?.complete();
    _downloadCompleter = null;
  }

  Future<void> _handleTransferComplete() async {
    if (_downloadBuffer.isEmpty || _downloadFilename.isEmpty) return;
    // Take ownership of this download BEFORE the first await: completion is
    // triggered both by the last chunk and by the watch's 0x13 packet, and
    // the second call must find nothing left to do (it used to write a
    // second, empty-named file or clobber the next download's buffer).
    final bytes = List<int>.from(_downloadBuffer);
    final filename = _downloadFilename;
    final completer = _downloadCompleter;
    _downloadBuffer.clear();
    _downloadFilename = '';
    _downloadCompleter = null;

    if (_tempName != null && _tempName == filename) {
      final c = _tempCompleter;
      _tempName = null;
      _tempCompleter = null;
      if (c != null && !c.isCompleted) c.complete(Uint8List.fromList(bytes));
      if (completer != null && !completer.isCompleted) completer.complete();
      return;
    }

    try {
      final dir = await getApplicationDocumentsDirectory();
      final recordingsDir = Directory('${dir.path}/recordings');
      if (!await recordingsDir.exists()) {
        await recordingsDir.create(recursive: true);
      }

      final file = File('${recordingsDir.path}/$filename');
      await file.writeAsBytes(bytes);

      final entries = await NotesStorage.loadAll();
      final idx = entries.indexWhere((e) => e.name == filename);
      if (idx >= 0 && entries[idx].recordedAt == null) {
        entries[idx].recordedAt = DateTime.now();
        await NotesStorage.save(entries[idx]);
      }

      print('[Notes] File downloaded: $filename (${file.path})');

      if (mounted) {
        final updated = Set<String>.from(state.downloadedFiles)..add(filename);
        state = state.copyWith(downloadedFiles: updated);
      }
    } finally {
      if (completer != null && !completer.isCompleted) completer.complete();
    }
  }

  Future<void> syncFromWatch() async {
    if (!_ble.isConnected) {
      state = state.copyWith(error: 'Watch not connected');
      return;
    }
    if (_downloadFilename.isNotEmpty || _batchRunning) return; // a download is running; sync after it
    _syncSessionId++;
    final sid = _syncSessionId;
    state = state.copyWith(isSyncing: true, error: null, downloadingIdx: null, downloadingName: null);
    await _ble.requestFileList();

    // Timeout if firmware doesn't respond within 10s
    Future.delayed(const Duration(seconds: 10), () {
      try {
        if (state.isSyncing && _syncSessionId == sid) {
          state = state.copyWith(isSyncing: false, error: 'Sync timed out');
        }
      } catch (_) {
        // Notifier was disposed — ignore
      }
    });
  }

  Future<void> downloadFile(String filename, int index) async {
    if (!_ble.isConnected) {
      state = state.copyWith(error: 'Watch not connected');
      return;
    }
    if (_batchRunning) {
      state = state.copyWith(error: 'Recordings are syncing - this one is in the queue');
      return;
    }
    final size = state.recordings.where((r) => r.name == filename).firstOrNull?.size ?? 0;
    // Joining Wi-Fi takes a few seconds: only worth it for big files.
    if (_useWifi(size, 2 * 1024 * 1024)) {
      final dir = await getApplicationDocumentsDirectory();
      final recDir = Directory('${dir.path}/recordings');
      if (!await recDir.exists()) await recDir.create(recursive: true);
      state = state.copyWith(totalToDownload: 1, downloadedCount: 0, error: null);
      final done = await _downloadViaWifi(
          [MapEntry(index, RecordingEntry(name: filename, size: size))], recDir, () => false);
      if (done.contains(filename)) {
        if (mounted) {
          state = state.copyWith(
              downloadingIdx: null, downloadingName: null, downloadProgress: 0, totalToDownload: 0, transferVia: null);
        }
        return;
      }
    }
    // Wait for the Bluetooth slot, then take it with no await in between.
    while (_tempName != null || _downloadFilename.isNotEmpty || _batchRunning) {
      await Future.delayed(const Duration(milliseconds: 250));
    }
    if (!_ble.isConnected) return;
    final completer = _claim(filename);
    await _prio(true);
    state = state.copyWith(error: null, downloadingIdx: index, downloadingName: filename, downloadProgress: 0);
    try {
      await _ble.requestFileDownload(filename);
      await _openWindow();
    } catch (e) {
      _downloadBuffer.clear();
      _downloadFilename = '';
      _downloadCompleter = null;
      state = state.copyWith(error: 'Download failed: $e', downloadingIdx: null, downloadingName: null);
      await _prio(false);
      return;
    }
    // Firmware pushes chunk 0 automatically once the file is open (see
    // the DOWNLOAD_FILE handler in AmoledSmartWatchOS.ino) - we used to
    // also send an immediate requestNextChunk() here, but that raced
    // against firmware processing the DOWNLOAD_FILE command (two
    // separate BLE writes, no ordering guarantee between when each
    // side finishes handling its own), and a chunk request that arrived
    // before the download was marked active was silently dropped and
    // never retried.
    try {
      await completer.future.timeout(
        const Duration(seconds: 300),
        onTimeout: () {
          print('[Notes] Download timeout for $filename');
          if (identical(_downloadCompleter, completer)) {
            _downloadBuffer.clear();
            _downloadFilename = '';
            _downloadCompleter = null;
          }
        },
      );
    } catch (_) {}
    await _prio(false);
    state = state.copyWith(downloadingIdx: null, downloadingName: null, downloadProgress: 0, transferVia: null);
  }

  /// Downloads a file from the watch's /Recordings into memory without
  /// adding it to the memo list (used for voice replies). Waits for a
  /// memo download that is already running.
  Future<Uint8List> fetchTemp(String filename, {Duration timeout = const Duration(seconds: 90)}) async {
    if (!_ble.isConnected) throw Exception('Watch not connected');
    final until = DateTime.now().add(const Duration(seconds: 15));
    while (_downloadFilename.isNotEmpty || _tempName != null) {
      if (DateTime.now().isAfter(until)) throw Exception('Phone is busy syncing recordings');
      await Future.delayed(const Duration(milliseconds: 200));
    }
    // Claim the slot - no await between the check above and here.
    final temp = _tempCompleter = Completer<Uint8List>();
    _tempName = filename;
    _claim(filename);
    try {
      await _prio(true);
      await _ble.requestFileDownload(filename);
      await _openWindow();
      return await temp.future.timeout(timeout);
    } on TimeoutException {
      if (_downloadFilename == filename) {
        _downloadBuffer.clear();
        _downloadFilename = '';
        _downloadCompleter = null;
      }
      _tempName = null;
      _tempCompleter = null;
      throw Exception('The recording took too long to arrive');
    } catch (e) {
      // Couldn't even start (write failed): free the slot.
      if (_tempName == filename) {
        if (_downloadFilename == filename) {
          _downloadBuffer.clear();
          _downloadFilename = '';
          _downloadCompleter = null;
        }
        _tempName = null;
        _tempCompleter = null;
      }
      rethrow;
    } finally {
      _prio(false);
    }
  }

  /// Adds an audio file from the phone to the watch's recordings (WAV or
  /// MP3 - what the watch can play). Over Wi-Fi when possible (any size, no
  /// confirmation), otherwise Bluetooth (max 4 MB, confirmed on the watch).
  /// Returns a message for the user.
  Future<String> addRecording(File src, String originalName) async {
    if (!_ble.isConnected) throw Exception('Watch not connected');
    final lower = originalName.toLowerCase();
    if (!lower.endsWith('.wav') && !lower.endsWith('.mp3')) {
      throw Exception('The watch plays WAV and MP3 files');
    }
    final ext = lower.substring(lower.length - 4);
    var base = originalName.substring(0, originalName.length - 4)
        .replaceAll(RegExp(r'[^A-Za-z0-9 _\-]'), '_')
        .replaceAll(RegExp(r'^[_. ]+'), '');
    if (base.length > 40) base = base.substring(0, 40);
    base = base.trim();
    if (base.isEmpty) base = 'audio';
    // Not the name of something already on the watch.
    final taken = state.recordings.map((r) => r.name.toLowerCase()).toSet();
    var name = '$base$ext';
    for (var i = 2; taken.contains(name.toLowerCase()) && i < 100; i++) {
      name = '${base}_$i$ext';
    }
    final size = await src.length();

    state = state.copyWith(uploadingName: name, uploadProgress: 0, error: null);
    String msg;
    try {
      final saved = await _sendToWatch(name, src, size);
      if (saved == null) {
        // Only on the watch once the user taps Add there: the next sync picks
        // it up (and downloads it back - the watch may have renamed it).
        msg = 'Sent - tap Add on the watch to keep "$name"';
        Future.delayed(const Duration(seconds: 8), () {
          if (mounted && _ble.isConnected) syncFromWatch();
        });
        return msg;
      }
      msg = 'Added "$saved" to the watch';
      name = saved;
      // Keep a copy here so the next sync doesn't download it back.
      final dir = await getApplicationDocumentsDirectory();
      final recDir = Directory('${dir.path}/recordings');
      if (!await recDir.exists()) await recDir.create(recursive: true);
      await src.copy('${recDir.path}/$name');
      await NotesStorage.save(RecordingEntry(name: name, size: size, recordedAt: DateTime.now()));
      if (mounted) {
        final entries = await NotesStorage.loadAll();
        state = state.copyWith(recordings: entries, downloadedFiles: Set<String>.from(state.downloadedFiles)..add(name));
      }
    } finally {
      if (mounted) state = state.copyWith(uploadingName: null, uploadProgress: 0);
    }
    Future.delayed(const Duration(seconds: 2), () {
      if (mounted && _ble.isConnected) syncFromWatch();
    });
    return msg;
  }

  /// Puts [src] in the watch's recordings as [name]: over Wi-Fi when possible
  /// (any size, no confirmation) - returns the name the watch saved it as -
  /// otherwise Bluetooth (max 4 MB) - returns null: it's kept once the user
  /// taps Add on the watch. Progress goes to state.uploadProgress.
  Future<String?> _sendToWatch(String name, File src, int size) async {
    String? saved;
    final big = size > BleService.maxRecordingSize;
    if (_wifiAllowed() && _ble.fastTransfers && (big || size > 512 * 1024) &&
        (big || DateTime.now().isAfter(_wifiBackoffUntil))) {
      var acquired = false;
      try {
        final s = await _acquireWifi();
        acquired = true;
        saved = await s.upload(name, src, onProgress: (sent, total) {
          if (mounted) state = state.copyWith(uploadProgress: total > 0 ? sent / total : 0);
        });
      } catch (e) {
        if (big) rethrow;
        print('[Notes] Wi-Fi upload failed, using Bluetooth: $e');
      } finally {
        if (acquired) await _releaseWifi();
      }
    }
    if (saved != null) return saved;
    if (big) throw Exception('Over 4 MB: needs Wi-Fi (set up Wi-Fi on the watch, same network as the phone)');
    await _prio(true);
    try {
      await _ble.sendRecording(name, await src.readAsBytes(), onProgress: (sent) {
        if (mounted) state = state.copyWith(uploadProgress: sent / size);
      });
    } finally {
      await _prio(false);
    }
    return null;
  }

  // ---- Memos recorded on the phone ----------------------------------------------

  static const _pendingKey = 'notes_pending_to_watch';
  bool _flushing = false;

  /// File name for a memo recorded on the phone now ("phone_261008_171230.wav").
  static String phoneMemoName([DateTime? at]) {
    final t = at ?? DateTime.now();
    String two(int v) => v.toString().padLeft(2, '0');
    return 'phone_${two(t.year % 100)}${two(t.month)}${two(t.day)}_${two(t.hour)}${two(t.minute)}${two(t.second)}.wav';
  }

  /// Where the phone recorder writes [name] (the memos folder).
  static Future<String> phoneMemoPath(String name) async => (await _localFile(name)).path;

  /// A memo just recorded with the phone's microphone (already at
  /// phoneMemoPath(name)): it's a memo here right away, gets transcribed and
  /// summarized when auto-processing is on, and is copied to the watch -
  /// now, or the next time the watch connects. Returns a message for the user.
  Future<String> addPhoneRecording(String name, int durationMs) async {
    final f = await _localFile(name);
    if (!await f.exists()) throw Exception('The recording was not saved');
    final size = await f.length();
    await NotesStorage.save(RecordingEntry(
        name: name, size: size, recordedAt: DateTime.now(), durationSec: (durationMs / 1000).round()));
    final entries = await NotesStorage.loadAll();
    if (mounted) {
      state = state.copyWith(recordings: entries, downloadedFiles: Set<String>.from(state.downloadedFiles)..add(name));
    }
    final prefs = await SharedPreferences.getInstance();
    await prefs.setStringList(_pendingKey, [...?prefs.getStringList(_pendingKey), name]);

    if (_autoProcess()) Future(() => processRecording(name));

    if (!_ble.isConnected) return 'Memo saved - it goes to the watch when it connects';
    final sent = await _flushPending();
    return sent ?? 'Memo saved';
  }

  /// Copies phone memos the watch doesn't have yet ([onWatch]: its current
  /// file list, when known). One at a time, when the Bluetooth link is free.
  /// Returns a message about the last one, or null if nothing was sent.
  Future<String?> _flushPending({Set<String>? onWatch}) async {
    if (_flushing || !_ble.isConnected) return null;
    _flushing = true;
    String? msg;
    var sentAny = false;
    try {
      final prefs = await SharedPreferences.getInstance();
      final pending = [...?prefs.getStringList(_pendingKey)];
      for (final name in List<String>.from(pending)) {
        final f = await _localFile(name);
        if ((onWatch?.contains(name) ?? false) || !await f.exists()) {
          pending.remove(name);
          await prefs.setStringList(_pendingKey, pending);
          continue;
        }
        while (_tempName != null || _downloadFilename.isNotEmpty || _batchRunning || state.uploadingName != null) {
          await Future.delayed(const Duration(milliseconds: 250));
          if (!_ble.isConnected || !mounted) return msg;
        }
        state = state.copyWith(uploadingName: name, uploadProgress: 0);
        try {
          final saved = await _sendToWatch(name, f, await f.length());
          msg = saved != null ? 'Memo saved and copied to the watch' : 'Memo saved - tap Add on the watch to keep it there too';
          sentAny = true;
        } catch (e) {
          msg = 'Memo saved on the phone - not copied to the watch: ${e.toString().replaceFirst('Exception: ', '')}';
          break;
        } finally {
          if (mounted) state = state.copyWith(uploadingName: null, uploadProgress: 0);
        }
        // One try each: a Bluetooth copy the user declines on the watch isn't re-sent.
        pending.remove(name);
        await prefs.setStringList(_pendingKey, pending);
      }
    } finally {
      _flushing = false;
    }
    if (sentAny) {
      Future.delayed(const Duration(seconds: 8), () {
        if (mounted && _ble.isConnected) syncFromWatch();
      });
    }
    return msg;
  }

  Future<void> deleteFromWatch(String filename) async {
    if (!_ble.isConnected) return;
    await _ble.requestFileDelete(filename);
    await NotesStorage.delete(filename);
    final dir = await getApplicationDocumentsDirectory();
    final file = File('${dir.path}/recordings/$filename');
    if (await file.exists()) await file.delete();
    final updated = Set<String>.from(state.downloadedFiles)..remove(filename);
    final entries = await NotesStorage.loadAll();
    state = state.copyWith(recordings: entries, downloadedFiles: updated);
  }

  /// Transcribes (whisper) and summarizes (on-device LLM) a downloaded
  /// recording, then sends the transcript back to the watch.
  Future<void> processRecording(String filename) async {
    if (state.processingTranscript || state.pipelineRunning) return;
    try {
      await _process(filename);
    } catch (e) {
      if (mounted) state = state.copyWith(error: e.toString().replaceFirst('Exception: ', ''));
    } finally {
      _processingDone();
    }
  }

  void _processingDone() {
    if (mounted) {
      state = state.copyWith(processingTranscript: false, processingName: null, processingStage: '', processingProgress: -1);
    }
  }

  static Future<File> _localFile(String filename) async {
    final dir = await getApplicationDocumentsDirectory();
    return File('${dir.path}/recordings/$filename');
  }

  /// whisper's markers for silence / noise only ("[BLANK_AUDIO]", "(music)").
  static bool _isNonSpeech(String t) =>
      t.replaceAll(RegExp(r'\[[^\]]*\]|\([^)]*\)|[*♪]'), '').trim().length < 2;

  /// Seconds of audio in a WAV or MP3 file, 0 if unknown.
  static Future<int> _audioSeconds(File f) async =>
      f.path.toLowerCase().endsWith('.mp3') ? _mp3Seconds(f) : _wavSeconds(f);

  /// MP3 length from the first frame header (bitrate) and the file size;
  /// exact for constant bitrate, an estimate for VBR. Skips an ID3v2 tag.
  static Future<int> _mp3Seconds(File f) async {
    RandomAccessFile? raf;
    try {
      raf = await f.open();
      final len = await raf.length();
      var head = await raf.read(10);
      var start = 0;
      if (head.length == 10 && head[0] == 0x49 && head[1] == 0x44 && head[2] == 0x33) {
        start = 10 + ((head[6] & 0x7F) << 21 | (head[7] & 0x7F) << 14 | (head[8] & 0x7F) << 7 | (head[9] & 0x7F));
      }
      await raf.setPosition(start);
      final buf = await raf.read(4096);
      const rates = [0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0];  // MPEG1 L3
      const rates2 = [0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0];     // MPEG2/2.5 L3
      for (var i = 0; i + 3 < buf.length; i++) {
        if (buf[i] != 0xFF || (buf[i + 1] & 0xE0) != 0xE0) continue;
        final version = (buf[i + 1] >> 3) & 3;   // 3 = MPEG1
        final layer = (buf[i + 1] >> 1) & 3;     // 1 = layer III
        final bi = (buf[i + 2] >> 4) & 0xF;
        if (layer != 1 || version == 1 || bi == 0 || bi == 15) continue;
        final kbps = version == 3 ? rates[bi] : rates2[bi];
        if (kbps <= 0) continue;
        return ((len - start - i) * 8 / (kbps * 1000)).round();
      }
      return 0;
    } catch (_) {
      return 0;
    } finally {
      await raf?.close();
    }
  }

  /// Seconds of audio in a WAV file (from its header), 0 if unknown.
  static Future<int> _wavSeconds(File f) async {
    RandomAccessFile? raf;
    try {
      raf = await f.open();
      final h = await raf.read(64);
      final len = await raf.length();
      if (h.length < 44 || String.fromCharCodes(h.sublist(0, 4)) != 'RIFF' ||
          String.fromCharCodes(h.sublist(8, 12)) != 'WAVE') {
        return 0;
      }
      final bd = ByteData.sublistView(Uint8List.fromList(h));
      final byteRate = bd.getUint32(28, Endian.little);
      return byteRate > 0 ? ((len - 44) / byteRate).round() : 0;
    } catch (_) {
      return 0;
    } finally {
      await raf?.close();
    }
  }

  /// Applies a summary result (see GemmaService.summarizeAndOrganize).
  static RecordingEntry _apply(RecordingEntry e, Map<String, dynamic> r, {String? transcript}) {
    List<String> list(String k) => (r[k] as List<dynamic>?)?.map((x) => x.toString()).toList() ?? const [];
    return e.copyWith(
      transcript: transcript,
      summary: (r['summary'] as String?) ?? '',
      importance: (r['importance'] as int?) ?? 3,
      tags: list('tags'),
      title: (r['title'] as String?) ?? '',
      topics: list('topics'),
      actions: list('actions'),
      people: list('people'),
      recordedAt: e.recordedAt ?? DateTime.now(),
      processedAt: DateTime.now(),
    );
  }

  Future<void> _storeResult(String filename, Map<String, dynamic> result,
      {String? transcript, String? language, bool? languageSet, bool? noSpeech}) async {
    final entries = await NotesStorage.loadAll();
    final idx = entries.indexWhere((e) => e.name == filename);
    if (idx < 0) return;
    var e = _apply(entries[idx], result, transcript: transcript)
        .copyWith(language: language, languageSet: languageSet, noSpeech: noSpeech);
    if (e.durationSec == 0) {
      final secs = await _audioSeconds(await _localFile(filename));
      if (secs > 0) e = e.copyWith(durationSec: secs);
    }
    entries[idx] = e;
    await NotesStorage.saveAll(entries);
    if (mounted) state = state.copyWith(recordings: List.from(entries));
  }

  /// Transcribe + summarize one memo; throws a readable error. The caller
  /// resets the processing state.
  /// [lang]: transcribe in this language (user correction) instead of the
  /// memo's chosen language / the setting.
  Future<void> _process(String filename, {String? lang}) async {
    final wav = await _localFile(filename);
    if (!await wav.exists()) throw Exception('Download the recording first.');
    final entry = (await NotesStorage.loadAll()).where((x) => x.name == filename).firstOrNull;
    final forced = lang ?? ((entry?.languageSet ?? false) ? entry!.language : null);
    state = state.copyWith(
      processingTranscript: true,
      processingName: filename,
      processingStage: 'Transcribing...',
      processingProgress: -1,
      error: null,
    );
    final transcript = await WhisperService.instance.transcribe(
      wav.path,
      lang: forced,
      onProgress: (p) {
        if (mounted) {
          state = state.copyWith(processingStage: 'Transcribing ${(p * 100).round()}%', processingProgress: p);
        }
      },
    );
    if (transcript.isEmpty || _isNonSpeech(transcript)) {
      // Remember it, so "Process everything" doesn't retry it every time.
      final all = await NotesStorage.loadAll();
      final i = all.indexWhere((x) => x.name == filename);
      if (i >= 0) {
        all[i] = all[i].copyWith(noSpeech: true, durationSec: all[i].durationSec > 0 ? null : await _audioSeconds(wav));
        await NotesStorage.saveAll(all);
        if (mounted) state = state.copyWith(recordings: List.from(all));
      }
      throw Exception('No speech recognized in this recording');
    }

    // The language: the one asked for, else guessed from the words.
    final detected = TextInsights.detectLanguage(transcript).code;
    final language = (forced != null && forced.isNotEmpty) ? forced : detected;

    if (mounted) state = state.copyWith(processingStage: 'Summarizing...', processingProgress: -1);
    final result = await GemmaService.instance
        .summarizeAndOrganize(transcript, language: language.isEmpty ? null : WhisperService.languageName(language));
    await _storeResult(filename, result,
        transcript: transcript, language: language, languageSet: forced != null && forced.isNotEmpty, noSpeech: false);

    // The watch keeps a .txt next to the recording.
    if (_ble.isConnected) {
      try {
        await _ble.sendTranscript(filename, transcript);
      } catch (e) {
        print('[Notes] sending transcript to watch failed: $e');
      }
    }
  }

  /// The memo is in another language than recognized: transcribe it again in
  /// [code], then summary, topics, to-dos and insights follow.
  Future<void> changeLanguage(String filename, String code) async {
    if (state.processingTranscript || state.pipelineRunning) {
      state = state.copyWith(error: 'Wait for the current transcription to finish');
      return;
    }
    // Remember the choice first, so even a failed run keeps it.
    final entries = await NotesStorage.loadAll();
    final idx = entries.indexWhere((e) => e.name == filename);
    if (idx >= 0) {
      entries[idx] = entries[idx].copyWith(language: code, languageSet: true);
      await NotesStorage.saveAll(entries);
      if (mounted) state = state.copyWith(recordings: List.from(entries));
    }
    try {
      await _process(filename, lang: code);
    } catch (e) {
      if (mounted) state = state.copyWith(error: e.toString().replaceFirst('Exception: ', ''));
    } finally {
      _processingDone();
    }
  }

  /// Re-runs only the summary (e.g. after installing a model).
  Future<void> resummarize(String filename) async {
    if (state.processingTranscript || state.pipelineRunning) return;
    try {
      await _summarizeOnly(filename);
    } catch (e) {
      if (mounted) state = state.copyWith(error: e.toString().replaceFirst('Exception: ', ''));
    } finally {
      _processingDone();
    }
  }

  Future<void> _summarizeOnly(String filename) async {
    final e = (await NotesStorage.loadAll()).where((x) => x.name == filename).firstOrNull;
    if (e == null || e.transcript.isEmpty) return;
    state = state.copyWith(processingTranscript: true, processingName: filename, processingStage: 'Summarizing...', processingProgress: -1);
    final lang = e.language.isNotEmpty ? e.language : TextInsights.detectLanguage(e.transcript).code;
    final result = await GemmaService.instance
        .summarizeAndOrganize(e.transcript, language: lang.isEmpty ? null : WhisperService.languageName(lang));
    await _storeResult(filename, result, language: lang);
  }

  // ---- one-tap pipeline -------------------------------------------------------------

  void cancelPipeline() {
    if (!state.pipelineRunning) return;
    _pipelineCancel = true;
    // Stops the download batch after the current file. Only then: bumping it
    // otherwise also disarms syncFromWatch's timeout (isSyncing stuck on).
    if (_batchRunning) _syncSessionId++;
    state = state.copyWith(pipelineStage: 'Stopping after the current step...');
  }

  void clearPipelineReport() => state = state.copyWith(pipelineReport: null);

  void _stage(int step, String text, {int? done, int? total}) {
    if (!mounted) return;
    state = state.copyWith(pipelineStep: step, pipelineStage: text, pipelineDone: done ?? 0, pipelineTotal: total ?? 0);
  }

  /// Everything in one go: get the list from the watch, download what's new,
  /// transcribe and summarize what isn't yet, then refresh the insights.
  /// [skipSync]: the list was just fetched (automatic run after a sync).
  /// [quiet]: no report when there was nothing to do.
  Future<void> runPipeline({bool skipSync = false, bool quiet = false}) async {
    if (state.pipelineRunning) return;
    // Wait for a single transcription the user started.
    if (state.processingTranscript) {
      if (!quiet) state = state.copyWith(error: 'Wait for the current transcription to finish');
      return;
    }
    _pipelineCancel = false;
    state = state.copyWith(
        pipelineRunning: true, pipelineReport: null, error: null, pipelineStep: 0, pipelineStage: 'Starting...');
    var downloaded = 0, transcribed = 0, summarized = 0, failed = 0;
    final notes = <String>[];
    try {
      // 1-2. list + download
      if (_ble.isConnected) {
        final before = <String>{};
        for (final r in state.recordings) {
          if (await (await _localFile(r.name)).exists()) before.add(r.name);
        }
        if (!skipSync) {
          _stage(0, 'Checking the watch...');
          while (_batchRunning && !_pipelineCancel) {
            await Future.delayed(const Duration(milliseconds: 300));
          }
          final waiter = _listWait = Completer<void>();
          try {
            await syncFromWatch();
            await waiter.future.timeout(const Duration(seconds: 12), onTimeout: () {});
          } finally {
            if (identical(_listWait, waiter)) _listWait = null;
          }
        }
        while (_batchRunning && !_pipelineCancel) {
          final n = state.totalToDownload;
          _stage(1, n > 0 ? 'Downloading ${state.downloadedCount + 1} of $n' : 'Downloading...',
              done: state.downloadedCount, total: n);
          await Future.delayed(const Duration(milliseconds: 300));
        }
        for (final r in state.recordings) {
          if (!before.contains(r.name) && await (await _localFile(r.name)).exists()) downloaded++;
        }
      } else {
        notes.add('watch not connected, processed what is on the phone');
      }
      if (_pipelineCancel) return;

      // Lengths for memos that don't have one yet (charts).
      final entries = await NotesStorage.loadAll();
      var changed = false;
      for (var i = 0; i < entries.length; i++) {
        if (entries[i].durationSec > 0) continue;
        final f = await _localFile(entries[i].name);
        if (!await f.exists()) continue;
        final secs = await _audioSeconds(f);
        if (secs > 0) {
          entries[i] = entries[i].copyWith(durationSec: secs);
          changed = true;
        }
      }
      if (changed) {
        await NotesStorage.saveAll(entries);
        if (mounted) state = state.copyWith(recordings: List.from(entries));
      }

      // 3. transcribe (+ summarize) what's downloaded and not transcribed
      final toTranscribe = <String>[];
      for (final r in entries) {
        // MP3s added from the phone are music/audio: whisper here only reads WAV.
        if (r.transcript.isEmpty && !r.noSpeech && !r.name.toLowerCase().endsWith('.mp3') &&
            await (await _localFile(r.name)).exists()) {
          toTranscribe.add(r.name);
        }
      }
      if (toTranscribe.isNotEmpty) {
        if (!await WhisperService.instance.isDownloaded()) {
          notes.add('${toTranscribe.length} memo(s) need a speech model (AI settings)');
        } else {
          for (var i = 0; i < toTranscribe.length && !_pipelineCancel; i++) {
            _stage(2, 'Transcribing ${i + 1} of ${toTranscribe.length}', done: i, total: toTranscribe.length);
            try {
              await _process(toTranscribe[i]);
              transcribed++;
            } catch (e) {
              failed++;
              print('[Notes] pipeline: ${toTranscribe[i]}: $e');
            } finally {
              _processingDone();
            }
          }
        }
      }
      if (_pipelineCancel) return;

      // 4. summaries missing or made before titles/topics existed
      final toSummarize = (await NotesStorage.loadAll())
          .where((r) => r.transcript.isNotEmpty && (r.summary.isEmpty || r.processedAt == null))
          .map((r) => r.name)
          .toList();
      for (var i = 0; i < toSummarize.length && !_pipelineCancel; i++) {
        _stage(3, 'Summarizing ${i + 1} of ${toSummarize.length}', done: i, total: toSummarize.length);
        try {
          await _summarizeOnly(toSummarize[i]);
          summarized++;
        } catch (e) {
          failed++;
          print('[Notes] pipeline summary: ${toSummarize[i]}: $e');
        } finally {
          _processingDone();
        }
      }
      if (_pipelineCancel) return;

      // 5. insights (search index and charts rebuild by themselves) + names
      _stage(4, 'Updating insights...');
      final hook = afterPipeline;
      if (hook != null && (transcribed + summarized) > 0) {
        try {
          await hook();
        } catch (e) {
          print('[Notes] pipeline names: $e');
        }
      }
    } catch (e) {
      notes.add(e.toString().replaceFirst('Exception: ', ''));
    } finally {
      final parts = <String>[
        if (downloaded > 0) '$downloaded downloaded',
        if (transcribed > 0) '$transcribed transcribed',
        if (summarized > 0) '$summarized summarized',
        if (failed > 0) '$failed failed',
      ];
      final nothing = parts.isEmpty && notes.isEmpty;
      final report = _pipelineCancel
          ? 'Stopped${parts.isEmpty ? '' : ' - ${parts.join(', ')}'}'
          : nothing
              ? 'Everything is up to date'
              : [if (parts.isNotEmpty) parts.join(', '), ...notes].join(' · ');
      _pipelineCancel = false;
      if (mounted) {
        state = state.copyWith(
          pipelineRunning: false,
          pipelineStage: '',
          pipelineDone: 0,
          pipelineTotal: 0,
          pipelineReport: quiet && nothing ? null : report,
        );
      }
    }
  }

  Future<void> toggleFavorite(String filename) async {
    final e = state.recordings.where((r) => r.name == filename).firstOrNull;
    if (e != null) await updateRecording(e.copyWith(favorite: !e.favorite));
  }

  Future<void> updateRecording(RecordingEntry entry) async {
    await NotesStorage.save(entry);
    final entries = await NotesStorage.loadAll();
    state = state.copyWith(recordings: entries);
  }

  Future<void> deleteLocal(String filename) async {
    final dir = await getApplicationDocumentsDirectory();
    final file = File('${dir.path}/recordings/$filename');
    if (await file.exists()) {
      await file.delete();
    }
    await NotesStorage.delete(filename);
    final updated = Set<String>.from(state.downloadedFiles)..remove(filename);
    final entries = await NotesStorage.loadAll();
    state = state.copyWith(recordings: entries, downloadedFiles: updated);
  }

  Future<void> clearAll() async {
    final dir = await getApplicationDocumentsDirectory();
    final recordingsDir = Directory('${dir.path}/recordings');
    if (await recordingsDir.exists()) {
      await recordingsDir.delete(recursive: true);
    }
    await NotesStorage.clearAll();
    state = state.copyWith(recordings: []);
  }

  @override
  void dispose() {
    _notesSub?.cancel();
    super.dispose();
  }
}

final StateNotifierProvider<NotesNotifier, NotesState> notesProvider =
    StateNotifierProvider<NotesNotifier, NotesState>((ref) {
  // ref.read, not watch: watching bleProvider rebuilt this notifier on
  // every battery/scan/media update, killing downloads in progress.
  final n = NotesNotifier(
    ref.read(bleServiceProvider),
    () => ref.read(settingsProvider).wifiTransfers,
    () => ref.read(settingsProvider).autoProcess,
  );
  // Name new theme collections with the on-device model after processing.
  n.afterPipeline = () => ref.read(collectionNamesProvider.notifier).nameMissing(ref.read(knowledgeProvider));
  return n;
});
