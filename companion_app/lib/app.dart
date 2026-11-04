import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'providers/settings_provider.dart';
import 'screens/onboarding_screen.dart';
import 'theme/app_theme.dart';
import 'screens/apps_screen.dart';
import 'screens/home_screen.dart';
import 'screens/notes_screen.dart';
import 'screens/settings_screen.dart';
import 'screens/updates_screen.dart';
import 'screens/faces_screen.dart';
import 'screens/camera_remote_screen.dart';
import 'services/camera_remote.dart';

/// The app's navigator: the watch can open a screen (the camera remote).
final appNavigatorKey = GlobalKey<NavigatorState>();

class AmoledCompanionApp extends StatelessWidget {
  const AmoledCompanionApp({super.key});

  @override
  Widget build(BuildContext context) {
    CameraRemote.instance.openScreen ??= () =>
        appNavigatorKey.currentState?.push(MaterialPageRoute(builder: (_) => const CameraRemoteScreen()));
    return MaterialApp(
      navigatorKey: appNavigatorKey,
      title: 'AmoledWatch',
      debugShowCheckedModeBanner: false,
      theme: AppTheme.dark,
      darkTheme: AppTheme.dark,
      themeMode: ThemeMode.dark,
      // iPad and big tablets: pages at a readable width, not stretched edge to edge
      builder: (context, child) => LayoutBuilder(
        builder: (context, c) => c.maxWidth <= 1100
            ? child!
            : ColoredBox(
                color: AppColors.bg,
                child: Center(
                  child: ConstrainedBox(constraints: const BoxConstraints(maxWidth: 1100), child: child),
                ),
              ),
      ),
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

/// The tabs: Home · Memos · Apps · Faces · Settings - at the bottom on phones,
/// down the side on iPad and tablets.
class RootShell extends StatefulWidget {
  const RootShell({super.key});

  @override
  State<RootShell> createState() => _RootShellState();
}

class _RootShellState extends State<RootShell> {
  int _tab = 0;
  final _built = <int>{0};

  static const _pages = <Widget>[HomeScreen(), NotesScreen(), AppsScreen(), FacesScreen(), SettingsScreen()];

  void selectTab(int i) => setState(() {
    _tab = i;
    _built.add(i);
  });

  static const _rail = [
    (Icons.home_outlined, Icons.home_rounded, 'Home'),
    (Icons.graphic_eq_rounded, Icons.graphic_eq_rounded, 'Memos'),
    (Icons.apps_outlined, Icons.apps_rounded, 'Apps'),
    (Icons.palette_outlined, Icons.palette, 'Faces'),
    (Icons.settings_outlined, Icons.settings_rounded, 'Settings'),
  ];

  @override
  Widget build(BuildContext context) {
    final pages = IndexedStack(
      index: _tab,
      // Tabs are built the first time they're opened, then kept alive.
      children: [for (var i = 0; i < _pages.length; i++) _built.contains(i) ? _pages[i] : const SizedBox.shrink()],
    );
    // iPad, tablets, landscape: the tabs down the side
    if (MediaQuery.sizeOf(context).width >= 720) {
      return Scaffold(
        body: Row(
          children: [
            SafeArea(
              right: false,
              child: NavigationRail(
                selectedIndex: _tab,
                onDestinationSelected: selectTab,
                labelType: NavigationRailLabelType.all,
                backgroundColor: AppColors.bg,
                destinations: [
                  for (final (icon, sel, label) in _rail)
                    NavigationRailDestination(icon: Icon(icon), selectedIcon: Icon(sel), label: Text(label)),
                ],
              ),
            ),
            const VerticalDivider(width: 1),
            Expanded(child: pages),
          ],
        ),
      );
    }
    return Scaffold(
      body: pages,
      bottomNavigationBar: NavigationBar(
        selectedIndex: _tab,
        onDestinationSelected: selectTab,
        destinations: const [
          NavigationDestination(icon: Icon(Icons.home_outlined), selectedIcon: Icon(Icons.home_rounded), label: 'Home'),
          NavigationDestination(icon: Icon(Icons.graphic_eq_rounded), label: 'Memos'),
          NavigationDestination(icon: Icon(Icons.apps_outlined), selectedIcon: Icon(Icons.apps_rounded), label: 'Apps'),
          NavigationDestination(icon: Icon(Icons.palette_outlined), selectedIcon: Icon(Icons.palette), label: 'Faces'),
          NavigationDestination(
            icon: Icon(Icons.settings_outlined),
            selectedIcon: Icon(Icons.settings_rounded),
            label: 'Settings',
          ),
        ],
      ),
    );
  }
}

/// Lets any screen switch tabs (e.g. the dashboard's "Memos" tile).
class TabSwitcher {
  static const home = 0, memos = 1, apps = 2, faces = 3, settings = 4;

  static void open(BuildContext context, int tab) {
    context.findAncestorStateOfType<_RootShellState>()?.selectTab(tab);
  }
}
