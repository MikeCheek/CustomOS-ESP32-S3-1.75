import 'dart:isolate';

import 'package:archive/archive_io.dart';

/// Unpacks [archive] into [outDir] on a background isolate.
///
/// Top level on purpose: a closure made inside a method (an install, a
/// download job) carries that method's whole context chain to the isolate -
/// in the app that reached the WhisperService and its Futures, which can't
/// be sent ("object is unsendable - _Future"), so every voice and speech
/// model install failed. Here the closure holds only the two paths.
Future<void> extractArchive(String archive, String outDir) => Isolate.run(() => extractFileToDisk(archive, outDir));
