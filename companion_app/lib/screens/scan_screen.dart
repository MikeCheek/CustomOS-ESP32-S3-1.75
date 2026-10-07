import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';
import '../providers/ble_provider.dart';
import '../theme/app_theme.dart';

class ScanScreen extends ConsumerStatefulWidget {
  const ScanScreen({super.key});

  @override
  ConsumerState<ScanScreen> createState() => _ScanScreenState();
}

class _ScanScreenState extends ConsumerState<ScanScreen> {
  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addPostFrameCallback((_) {
      ref.read(bleProvider.notifier).startScan();
    });
  }

  @override
  Widget build(BuildContext context) {
    final ble = ref.watch(bleProvider);

    return Scaffold(
      appBar: AppBar(
        title: const Text('Scan for Watch'),
        actions: [
          if (ble.isScanning)
            const Padding(
              padding: EdgeInsets.all(16),
              child: SizedBox(
                width: 20,
                height: 20,
                child: CircularProgressIndicator(
                  strokeWidth: 2,
                  color: AppColors.accent,
                ),
              ),
            )
          else
            IconButton(
              icon: const Icon(Icons.refresh),
              onPressed: () => ref.read(bleProvider.notifier).startScan(),
            ),
        ],
      ),
      body: ble.scanResults.isEmpty
          ? Center(
              child: ble.isScanning
                  ? const Column(
                      mainAxisSize: MainAxisSize.min,
                      children: [
                        CircularProgressIndicator(color: AppColors.accent),
                        SizedBox(height: 16),
                        Text('Scanning...', style: TextStyle(color: AppColors.textDim)),
                      ],
                    )
                  : const Text('No devices found\nTap refresh to scan again',
                      textAlign: TextAlign.center,
                      style: TextStyle(color: AppColors.textDim)),
            )
          : ListView.builder(
              padding: const EdgeInsets.all(16),
              itemCount: ble.scanResults.length,
              itemBuilder: (context, index) {
                final result = ble.scanResults[index];
                final device = result.device;
                final name = result.advertisementData.advName;
                final isWatch = name.toLowerCase().contains('amoled') ||
                    name.toLowerCase().contains('watch');
                final rssi = result.rssi;

                return Padding(
                  padding: const EdgeInsets.only(bottom: 8),
                  child: Material(
                    color: AppColors.surface,
                    borderRadius: BorderRadius.circular(12),
                    child: InkWell(
                      borderRadius: BorderRadius.circular(12),
                      onTap: () async {
                        await ref.read(bleProvider.notifier).stopScan();
                        await ref.read(bleProvider.notifier).connect(device);
                        if (context.mounted) Navigator.pop(context);
                      },
                      child: Padding(
                        padding: const EdgeInsets.all(16),
                        child: Row(
                          children: [
                            Container(
                              width: 40,
                              height: 40,
                              decoration: BoxDecoration(
                                color: isWatch
                                    ? AppColors.accent.withValues(alpha: 0.2)
                                    : AppColors.surfaceLight,
                                borderRadius: BorderRadius.circular(10),
                              ),
                              child: Icon(
                                isWatch ? Icons.watch : Icons.bluetooth,
                                color: isWatch ? AppColors.accent : AppColors.textDim,
                                size: 20,
                              ),
                            ),
                            const SizedBox(width: 12),
                            Expanded(
                              child: Column(
                                crossAxisAlignment: CrossAxisAlignment.start,
                                children: [
                                  Text(
                                    name.isNotEmpty ? name : 'Unknown Device',
                                    style: TextStyle(
                                      color: isWatch ? AppColors.text : AppColors.textDim,
                                      fontWeight: isWatch ? FontWeight.w600 : FontWeight.normal,
                                    ),
                                  ),
                                  const SizedBox(height: 2),
                                  Text(
                                    device.remoteId.toString(),
                                    style: const TextStyle(
                                      color: AppColors.textDim,
                                      fontSize: 12,
                                    ),
                                  ),
                                ],
                              ),
                            ),
                            Column(
                              children: [
                                Icon(
                                  _rssiIcon(rssi),
                                  color: _rssiColor(rssi),
                                  size: 18,
                                ),
                                const SizedBox(height: 2),
                                Text(
                                  '$rssi dBm',
                                  style: const TextStyle(
                                    color: AppColors.textDim,
                                    fontSize: 11,
                                  ),
                                ),
                              ],
                            ),
                          ],
                        ),
                      ),
                    ),
                  ),
                );
              },
            ),
    );
  }

  IconData _rssiIcon(int rssi) {
    if (rssi > -60) return Icons.signal_cellular_4_bar;
    if (rssi > -80) return Icons.signal_cellular_alt;
    return Icons.signal_cellular_alt_1_bar;
  }

  Color _rssiColor(int rssi) {
    if (rssi > -60) return AppColors.success;
    if (rssi > -80) return AppColors.warning;
    return AppColors.danger;
  }
}
