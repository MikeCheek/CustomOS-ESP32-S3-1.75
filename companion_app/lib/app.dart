import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'providers/settings_provider.dart';
import 'screens/onboarding_screen.dart';
import 'theme/app_theme.dart';
import 'screens/home_screen.dart';
import 'screens/notes_screen.dart';
import 'screens/settings_screen.dart';
import 'screens/updates_screen.dart';
import 'screens/watchface/watchface_library.dart';

class AmoledCompanionApp extends StatelessWidget {
  const AmoledCompanionApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'AmoledWatch',
      debugShowCheckedModeBanner: false,
      theme: AppTheme.dark,
      darkTheme: AppTheme.dark,
      themeMode: ThemeMode.dark,
      home: const _Gate(),
    );
  }
}

/// First run: the setup flow; afterwards the main tabs.
class _Gate extends ConsumerWidget {
  const _Gate();

  @override
  Widget build(BuildContext context, WidgetRef ref) {
    final s = ref.watch(settingsProvider);
    if (!s.loaded) return const Scaffold(backgroundColor: AppColors.bg);
    return AnimatedSwitcher(
      duration: const Duration(milliseconds: 350),
      child: s.onboarded ? const UpdatePrompter(child: RootShell()) : const OnboardingScreen(),
    );
  }
}

/// Bottom-navigation shell: Watch · Recordings · Faces · Settings.
class RootShell extends StatefulWidget {
  const RootShell({super.key});

  @override
  State<RootShell> createState() => _RootShellState();
}

class _RootShellState extends State<RootShell> {
  int _tab = 0;
  final _built = <int>{0};

  static const _pages = <Widget>[
    HomeScreen(),
    NotesScreen(),
    WatchfaceLibrary(),
    SettingsScreen(),
  ];

  void selectTab(int i) => setState(() {
        _tab = i;
        _built.add(i);
      });

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      body: IndexedStack(
        index: _tab,
        // Tabs are built the first time they're opened, then kept alive.
        children: [
          for (var i = 0; i < _pages.length; i++) _built.contains(i) ? _pages[i] : const SizedBox.shrink(),
        ],
      ),
      bottomNavigationBar: NavigationBar(
        selectedIndex: _tab,
        onDestinationSelected: selectTab,
        destinations: const [
          NavigationDestination(icon: Icon(Icons.watch_outlined), selectedIcon: Icon(Icons.watch), label: 'Watch'),
          NavigationDestination(icon: Icon(Icons.graphic_eq_rounded), label: 'Recordings'),
          NavigationDestination(icon: Icon(Icons.palette_outlined), selectedIcon: Icon(Icons.palette), label: 'Faces'),
          NavigationDestination(icon: Icon(Icons.tune_rounded), label: 'Settings'),
        ],
      ),
    );
  }
}

/// Lets any screen switch tabs (e.g. the dashboard's "Recordings" row).
class TabSwitcher {
  static void open(BuildContext context, int tab) {
    context.findAncestorStateOfType<_RootShellState>()?.selectTab(tab);
  }
}
