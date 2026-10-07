import 'package:flutter/material.dart';

/// Palette shared with the watch UI (AMOLED black, violet accent).
class AppColors {
  static const bg = Color(0xFF000000);
  static const surface = Color(0xFF101014);
  static const surfaceLight = Color(0xFF1A1A21);
  static const surfaceHigh = Color(0xFF23232C);
  static const border = Color(0xFF26262F);
  static const accent = Color(0xFF7B61FF);   // watch COLOR_ACCENT
  static const accent2 = Color(0xFF39D6FF);  // watch COLOR_ACCENT2
  static const accent3 = Color(0xFFFF5FA2);
  static const text = Color(0xFFF4F4F6);
  static const textDim = Color(0xFF8A8A96);
  static const textFaint = Color(0xFF55555F);
  static const success = Color(0xFF2EE6A0);
  static const danger = Color(0xFFFF5A6A);
  static const warning = Color(0xFFFFB547);
}

class AppTheme {
  static ThemeData get dark {
    const scheme = ColorScheme.dark(
      primary: AppColors.accent,
      onPrimary: Colors.white,
      secondary: AppColors.accent2,
      onSecondary: Colors.black,
      surface: AppColors.surface,
      onSurface: AppColors.text,
      error: AppColors.danger,
    );
    final base = ThemeData(brightness: Brightness.dark, colorScheme: scheme, useMaterial3: true);
    return base.copyWith(
      scaffoldBackgroundColor: AppColors.bg,
      canvasColor: AppColors.bg,
      textTheme: base.textTheme.apply(bodyColor: AppColors.text, displayColor: AppColors.text),
      appBarTheme: const AppBarTheme(
        backgroundColor: AppColors.bg,
        surfaceTintColor: Colors.transparent,
        foregroundColor: AppColors.text,
        elevation: 0,
        scrolledUnderElevation: 0,
        centerTitle: false,
        titleTextStyle: TextStyle(color: AppColors.text, fontSize: 20, fontWeight: FontWeight.w600, letterSpacing: -0.2),
      ),
      cardTheme: CardThemeData(
        color: AppColors.surface,
        elevation: 0,
        margin: EdgeInsets.zero,
        shape: RoundedRectangleBorder(
          borderRadius: BorderRadius.circular(22),
          side: const BorderSide(color: AppColors.border),
        ),
      ),
      navigationBarTheme: NavigationBarThemeData(
        backgroundColor: const Color(0xFF09090C),
        surfaceTintColor: Colors.transparent,
        height: 68,
        indicatorColor: AppColors.accent.withValues(alpha: 0.22),
        labelTextStyle: WidgetStateProperty.resolveWith((states) {
          final sel = states.contains(WidgetState.selected);
          return TextStyle(
            color: sel ? AppColors.text : AppColors.textDim,
            fontSize: 12,
            fontWeight: sel ? FontWeight.w600 : FontWeight.w500,
          );
        }),
        iconTheme: WidgetStateProperty.resolveWith((states) {
          final sel = states.contains(WidgetState.selected);
          return IconThemeData(color: sel ? AppColors.text : AppColors.textDim, size: 24);
        }),
      ),
      filledButtonTheme: FilledButtonThemeData(
        style: FilledButton.styleFrom(
          backgroundColor: AppColors.accent,
          foregroundColor: Colors.white,
          minimumSize: const Size(0, 50),
          padding: const EdgeInsets.symmetric(horizontal: 22),
          textStyle: const TextStyle(fontSize: 15, fontWeight: FontWeight.w600),
          shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(16)),
        ),
      ),
      elevatedButtonTheme: ElevatedButtonThemeData(
        style: ElevatedButton.styleFrom(
          backgroundColor: AppColors.accent,
          foregroundColor: Colors.white,
          elevation: 0,
          minimumSize: const Size(0, 50),
          padding: const EdgeInsets.symmetric(horizontal: 22),
          textStyle: const TextStyle(fontSize: 15, fontWeight: FontWeight.w600),
          shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(16)),
        ),
      ),
      outlinedButtonTheme: OutlinedButtonThemeData(
        style: OutlinedButton.styleFrom(
          foregroundColor: AppColors.text,
          side: const BorderSide(color: AppColors.border),
          minimumSize: const Size(0, 50),
          padding: const EdgeInsets.symmetric(horizontal: 22),
          textStyle: const TextStyle(fontSize: 15, fontWeight: FontWeight.w600),
          shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(16)),
        ),
      ),
      textButtonTheme: TextButtonThemeData(
        style: TextButton.styleFrom(foregroundColor: AppColors.accent),
      ),
      inputDecorationTheme: InputDecorationTheme(
        filled: true,
        fillColor: AppColors.surfaceLight,
        border: OutlineInputBorder(borderRadius: BorderRadius.circular(14), borderSide: BorderSide.none),
        enabledBorder: OutlineInputBorder(borderRadius: BorderRadius.circular(14), borderSide: BorderSide.none),
        focusedBorder: OutlineInputBorder(
          borderRadius: BorderRadius.circular(14),
          borderSide: const BorderSide(color: AppColors.accent),
        ),
        hintStyle: const TextStyle(color: AppColors.textDim),
        contentPadding: const EdgeInsets.symmetric(horizontal: 16, vertical: 14),
      ),
      switchTheme: SwitchThemeData(
        thumbColor: WidgetStateProperty.resolveWith((states) =>
            states.contains(WidgetState.selected) ? Colors.white : AppColors.textDim),
        trackColor: WidgetStateProperty.resolveWith((states) =>
            states.contains(WidgetState.selected) ? AppColors.accent : AppColors.surfaceHigh),
        trackOutlineColor: WidgetStateProperty.all(Colors.transparent),
      ),
      sliderTheme: const SliderThemeData(
        activeTrackColor: AppColors.accent,
        inactiveTrackColor: AppColors.surfaceHigh,
        thumbColor: Colors.white,
        overlayColor: Color(0x247B61FF),
      ),
      progressIndicatorTheme: const ProgressIndicatorThemeData(
        color: AppColors.accent,
        linearTrackColor: AppColors.surfaceHigh,
        circularTrackColor: AppColors.surfaceHigh,
      ),
      dividerTheme: const DividerThemeData(color: AppColors.border, thickness: 1, space: 1),
      listTileTheme: const ListTileThemeData(
        iconColor: AppColors.textDim,
        textColor: AppColors.text,
      ),
      snackBarTheme: SnackBarThemeData(
        behavior: SnackBarBehavior.floating,
        backgroundColor: AppColors.surfaceHigh,
        contentTextStyle: const TextStyle(color: AppColors.text, fontSize: 14),
        shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(14)),
      ),
      dialogTheme: DialogThemeData(
        backgroundColor: AppColors.surface,
        surfaceTintColor: Colors.transparent,
        shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(24)),
        titleTextStyle: const TextStyle(color: AppColors.text, fontSize: 19, fontWeight: FontWeight.w600),
        contentTextStyle: const TextStyle(color: AppColors.textDim, fontSize: 14, height: 1.4),
      ),
      bottomSheetTheme: const BottomSheetThemeData(
        backgroundColor: AppColors.surface,
        surfaceTintColor: Colors.transparent,
        showDragHandle: true,
      ),
      chipTheme: ChipThemeData(
        backgroundColor: AppColors.surfaceLight,
        side: BorderSide.none,
        labelStyle: const TextStyle(color: AppColors.text, fontSize: 12),
        shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(10)),
      ),
    );
  }
}
