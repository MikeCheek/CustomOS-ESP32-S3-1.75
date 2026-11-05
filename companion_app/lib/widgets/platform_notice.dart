import 'dart:io';

import 'package:flutter/material.dart';

import '../theme/app_theme.dart';
import 'common.dart';

/// On iPhone and iPad some of the phone link can't exist: iOS doesn't let
/// an app read other apps' notifications, control their music, change Do
/// Not Disturb or send texts on its own (the Apple Watch uses a private
/// system link for that). Screens show this instead of switches that would
/// do nothing.
bool get isIos => Platform.isIOS;

class IosLimitNotice extends StatelessWidget {
  final String title;
  final String text;
  final List<String> stillWorks;
  const IosLimitNotice({super.key, required this.title, required this.text, this.stillWorks = const []});

  @override
  Widget build(BuildContext context) {
    return Panel(
      color: AppColors.accent2.withValues(alpha: 0.08),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Row(
            children: [
              const IconChip(Icons.phone_iphone_rounded, color: AppColors.accent2),
              const SizedBox(width: 14),
              Expanded(
                child: Text(
                  title,
                  style: const TextStyle(color: AppColors.text, fontSize: 15, fontWeight: FontWeight.w600),
                ),
              ),
            ],
          ),
          const SizedBox(height: 10),
          Text(text, style: const TextStyle(color: AppColors.textDim, fontSize: 13, height: 1.4)),
          if (stillWorks.isNotEmpty) ...[
            const SizedBox(height: 12),
            const Text(
              'What still works on iPhone',
              style: TextStyle(color: AppColors.text, fontSize: 13, fontWeight: FontWeight.w600),
            ),
            const SizedBox(height: 6),
            for (final w in stillWorks)
              Padding(
                padding: const EdgeInsets.only(bottom: 4),
                child: Row(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    const Padding(
                      padding: EdgeInsets.only(top: 2, right: 8),
                      child: Icon(Icons.check_rounded, size: 16, color: AppColors.success),
                    ),
                    Expanded(
                      child: Text(w, style: const TextStyle(color: AppColors.textDim, fontSize: 13, height: 1.35)),
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
