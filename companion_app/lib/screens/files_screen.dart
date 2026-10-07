import 'dart:io';
import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:file_picker/file_picker.dart';
import '../providers/ble_provider.dart';
import '../providers/notes_provider.dart';
import '../widgets/common.dart';
import '../theme/app_theme.dart';

class FilesScreen extends ConsumerStatefulWidget {
  const FilesScreen({super.key});

  @override
  ConsumerState<FilesScreen> createState() => _FilesScreenState();
}

class _FilesScreenState extends ConsumerState<FilesScreen> {
  List<_PickedFile> _files = [];
  double _progress = 0;
  bool _sending = false;
  String _via = '';

  Future<void> _pickFiles() async {
    try {
      final result = await FilePicker.platform.pickFiles(
        allowMultiple: true,
        type: FileType.any,
      );
      if (result != null && result.files.isNotEmpty) {
        setState(() {
          _files = result.files.map((f) => _PickedFile(
            name: f.name,
            path: f.path ?? '',
            size: f.size,
          )).toList();
        });
      }
    } catch (e) {
      if (mounted) {
        ScaffoldMessenger.of(context).showSnackBar(
          SnackBar(content: Text('Error picking files: $e')),
        );
      }
    }
  }

  Future<void> _sendFile(_PickedFile file) async {
    if (file.path.isEmpty) return;
    try {
      final ble = ref.read(bleServiceProvider);
      final notes = ref.read(notesProvider.notifier);
      final bleMax = ble.maxFileSize;
      // Big files over Wi-Fi (any size, no confirmation on the watch);
      // small ones, or when Wi-Fi isn't available, over Bluetooth.
      final wifiOk = notes.wifiUsable(files: true);
      if (file.size > bleMax && !wifiOk) {
        if (mounted) {
          showSnack(context, ble.wifiFiles
              ? '${file.name} is over ${bleMax ~/ (1024 * 1024)} MB: set up Wi-Fi on the watch (same network as the phone) to send it'
              : '${file.name} is over 1 MB: update the watch firmware (2.8+) to send big files');
        }
        return;
      }
      setState(() { _sending = true; _progress = 0; _via = ''; });
      String msg;
      var sentOverWifi = false;
      if (wifiOk && (file.size > 512 * 1024 || file.size > bleMax)) {
        try {
          if (mounted) setState(() => _via = 'Wi-Fi');
          final saved = await notes.withWifi((s) => s.upload(file.name, File(file.path), toRoot: true,
              onProgress: (sent, total) {
                if (mounted) setState(() => _progress = total > 0 ? sent / total : 1);
              }));
          sentOverWifi = true;
          msg = '$saved is on the watch';
        } catch (e) {
          if (file.size > bleMax) rethrow;
          msg = '';
        }
      } else {
        msg = '';
      }
      if (!sentOverWifi) {
        if (mounted) setState(() { _via = 'Bluetooth'; _progress = 0; });
        final bytes = await File(file.path).readAsBytes();
        await ble.sendFileWithProgress(
          file.name, bytes,
          onProgress: (sentBytes) {
            if (mounted) setState(() => _progress = bytes.isEmpty ? 1 : sentBytes / bytes.length);
          },
        );
        msg = '${file.name} sent - accept it on the watch';
      }

      if (!mounted) return;
      setState(() { _sending = false; _progress = 1; });
      showSnack(context, msg);
    } catch (e) {
      if (mounted) setState(() => _sending = false);
      if (mounted) {
        ScaffoldMessenger.of(context).showSnackBar(
          SnackBar(content: Text('Error sending: $e')),
        );
      }
    }
  }

  @override
  Widget build(BuildContext context) {
    final ble = ref.watch(bleProvider);

    return Scaffold(
      appBar: AppBar(
        title: const Text('File Transfer'),
        actions: [
          IconButton(
            icon: const Icon(Icons.add),
            onPressed: _pickFiles,
            tooltip: 'Pick files',
          ),
        ],
      ),
      body: Column(
        children: [
          if (_sending)
            Container(
              padding: const EdgeInsets.all(16),
              child: Column(
                children: [
                  LinearProgressIndicator(
                    value: _progress,
                    backgroundColor: AppColors.surfaceLight,
                    color: AppColors.accent,
                  ),
                  const SizedBox(height: 8),
                  Text(
                    'Sending${_via.isNotEmpty ? ' over $_via' : ''}... ${(_progress * 100).round()}%',
                    style: const TextStyle(color: AppColors.textDim, fontSize: 12),
                  ),
                ],
              ),
            ),
          if (_files.isEmpty)
            Expanded(
              child: Center(
                child: Column(
                  mainAxisSize: MainAxisSize.min,
                  children: [
                    const Icon(Icons.folder_open, size: 64, color: AppColors.textDim),
                    const SizedBox(height: 16),
                    const Text('No files selected',
                        style: TextStyle(color: AppColors.textDim)),
                    const SizedBox(height: 8),
                    ElevatedButton.icon(
                      onPressed: _pickFiles,
                      icon: const Icon(Icons.add, size: 18),
                      label: const Text('Pick Files'),
                    ),
                  ],
                ),
              ),
            )
          else
            Expanded(
              child: ListView.builder(
                padding: const EdgeInsets.all(16),
                itemCount: _files.length,
                itemBuilder: (context, index) {
                  final file = _files[index];
                  final ext = file.name.split('.').last.toLowerCase();
                  final icon = _fileIcon(ext);

                  return Card(
                    margin: const EdgeInsets.only(bottom: 8),
                    child: ListTile(
                      leading: Container(
                        width: 40,
                        height: 40,
                        decoration: BoxDecoration(
                          color: AppColors.accent.withValues(alpha: 0.15),
                          borderRadius: BorderRadius.circular(10),
                        ),
                        child: Icon(icon, color: AppColors.accent, size: 20),
                      ),
                      title: Text(file.name,
                          style: const TextStyle(color: AppColors.text, fontSize: 14)),
                      subtitle: Text(_formatSize(file.size),
                          style: const TextStyle(color: AppColors.textDim, fontSize: 12)),
                      trailing: IconButton(
                        icon: const Icon(Icons.send, color: AppColors.accent, size: 18),
                        onPressed: ble.isConnected && !_sending
                            ? () => _sendFile(file)
                            : null,
                      ),
                    ),
                  );
                },
              ),
            ),
          if (_files.isNotEmpty)
            Padding(
              padding: const EdgeInsets.all(16),
              child: Row(
                children: [
                  Expanded(
                    child: OutlinedButton.icon(
                      onPressed: _pickFiles,
                      icon: const Icon(Icons.add, size: 18),
                      label: const Text('Add More'),
                    ),
                  ),
                  const SizedBox(width: 12),
                  Expanded(
                    child: ElevatedButton.icon(
                      onPressed: ble.isConnected && !_sending && _files.isNotEmpty
                          ? () async {
                              for (final file in _files) {
                                await _sendFile(file);
                              }
                            }
                          : null,
                      icon: const Icon(Icons.send, size: 18),
                      label: Text('Send All (${_files.length})'),
                    ),
                  ),
                ],
              ),
            ),
        ],
      ),
    );
  }

  IconData _fileIcon(String ext) {
    switch (ext) {
      case 'jpg': case 'jpeg': case 'png': case 'gif': case 'webp':
        return Icons.image;
      case 'mp3': case 'wav': case 'ogg': case 'm4a':
        return Icons.audio_file;
      case 'mp4': case 'avi': case 'mov': case 'mkv':
        return Icons.video_file;
      case 'txt': case 'md': case 'json':
        return Icons.text_snippet;
      case 'pdf':
        return Icons.picture_as_pdf;
      default:
        return Icons.insert_drive_file;
    }
  }

  String _formatSize(int bytes) {
    if (bytes < 1024) return '$bytes B';
    if (bytes < 1024 * 1024) return '${(bytes / 1024).toStringAsFixed(1)} KB';
    return '${(bytes / (1024 * 1024)).toStringAsFixed(1)} MB';
  }
}

class _PickedFile {
  final String name;
  final String path;
  final int size;
  _PickedFile({required this.name, required this.path, required this.size});
}
