import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:permission_handler/permission_handler.dart';
import '../providers/ble_provider.dart';
import '../services/ble_protocol.dart';
import '../theme/app_theme.dart';

/// Picks phone contacts (real address book, READ_CONTACTS) and sends them
/// to the watch, which keeps them on its SD card (/contacts.json).
class ContactsScreen extends ConsumerStatefulWidget {
  const ContactsScreen({super.key});

  @override
  ConsumerState<ContactsScreen> createState() => _ContactsScreenState();
}

class _ContactsScreenState extends ConsumerState<ContactsScreen> {
  List<ContactEntry> _allContacts = [];
  final Set<int> _selected = {};
  bool _loading = true;
  bool _sending = false;
  String? _problem;
  String _query = '';

  @override
  void initState() {
    super.initState();
    _load();
  }

  Future<void> _load() async {
    setState(() { _loading = true; _problem = null; });
    final status = await Permission.contacts.request();
    if (!status.isGranted) {
      if (!mounted) return;
      setState(() {
        _loading = false;
        _problem = status.isPermanentlyDenied
            ? 'Contacts permission was denied. Enable it in Android settings.'
            : 'Contacts permission is needed to send contacts to the watch.';
      });
      return;
    }
    try {
      final raw = await ref.read(nativeServiceProvider).getContacts();
      final list = raw
          .map((m) => ContactEntry(name: m['name'] ?? '', phone: m['phone'] ?? '', email: m['email'] ?? ''))
          .where((c) => c.name.isNotEmpty || c.phone.isNotEmpty)
          .toList();
      if (!mounted) return;
      setState(() { _allContacts = list; _selected.clear(); _loading = false; });
    } catch (e) {
      if (!mounted) return;
      setState(() { _loading = false; _problem = 'Could not read contacts: $e'; });
    }
  }

  List<int> get _visible {
    if (_query.isEmpty) return List.generate(_allContacts.length, (i) => i);
    final q = _query.toLowerCase();
    return [
      for (var i = 0; i < _allContacts.length; i++)
        if (_allContacts[i].name.toLowerCase().contains(q) || _allContacts[i].phone.contains(q)) i
    ];
  }

  Future<void> _send() async {
    final selected = _selected.map((i) => _allContacts[i]).toList()
      ..sort((a, b) => a.name.toLowerCase().compareTo(b.name.toLowerCase()));
    setState(() => _sending = true);
    String msg;
    try {
      await ref.read(bleServiceProvider).sendContacts(selected);
      // encodeContacts() drops contacts from the end if the list is
      // bigger than the watch's 8 KB buffer - tell the user how many fit.
      final sent = BleProtocol.contactsThatFit(selected);
      msg = sent < selected.length
          ? 'Sent $sent of ${selected.length} contacts (watch limit reached)'
          : '${selected.length} contacts sent!';
    } catch (e) {
      msg = 'Sending failed: $e';
    }
    if (!mounted) return;
    setState(() => _sending = false);
    ScaffoldMessenger.of(context).showSnackBar(SnackBar(content: Text(msg)));
  }

  String _initials(String name) {
    final parts = name.trim().split(RegExp(r'\s+')).where((p) => p.isNotEmpty).take(2);
    final s = parts.map((p) => p.characters.first.toUpperCase()).join();
    return s.isEmpty ? '#' : s;
  }

  @override
  Widget build(BuildContext context) {
    final ble = ref.watch(bleProvider);
    final visible = _visible;
    final allVisibleSelected = visible.isNotEmpty && visible.every(_selected.contains);

    return Scaffold(
      appBar: AppBar(
        title: const Text('Contacts'),
        actions: [
          if (_selected.isNotEmpty)
            TextButton(
              onPressed: ble.isConnected && !_sending ? _send : null,
              child: _sending
                  ? const SizedBox(width: 18, height: 18, child: CircularProgressIndicator(strokeWidth: 2))
                  : Text('Send (${_selected.length})', style: const TextStyle(color: AppColors.accent)),
            ),
        ],
      ),
      body: _loading
          ? const Center(child: CircularProgressIndicator())
          : _problem != null
              ? Center(
                  child: Padding(
                    padding: const EdgeInsets.all(24),
                    child: Column(mainAxisSize: MainAxisSize.min, children: [
                      Text(_problem!, textAlign: TextAlign.center, style: const TextStyle(color: AppColors.textDim)),
                      const SizedBox(height: 16),
                      TextButton(
                        onPressed: () async {
                          if (await Permission.contacts.isPermanentlyDenied) {
                            await openAppSettings();
                          } else {
                            await _load();
                          }
                        },
                        child: const Text('Grant access', style: TextStyle(color: AppColors.accent)),
                      ),
                    ]),
                  ),
                )
              : Column(
                  children: [
                    Padding(
                      padding: const EdgeInsets.fromLTRB(16, 8, 16, 0),
                      child: TextField(
                        decoration: const InputDecoration(
                          hintText: 'Search',
                          prefixIcon: Icon(Icons.search),
                          isDense: true,
                        ),
                        onChanged: (v) => setState(() => _query = v.trim()),
                      ),
                    ),
                    Container(
                      padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 8),
                      child: Row(
                        children: [
                          Expanded(
                            child: Text(
                              '${_selected.length} of ${_allContacts.length} selected',
                              style: const TextStyle(color: AppColors.textDim, fontSize: 13),
                            ),
                          ),
                          TextButton(
                            onPressed: visible.isEmpty
                                ? null
                                : () => setState(() {
                                      if (allVisibleSelected) {
                                        _selected.removeAll(visible);
                                      } else {
                                        _selected.addAll(visible);
                                      }
                                    }),
                            child: Text(
                              allVisibleSelected ? 'Deselect All' : 'Select All',
                              style: const TextStyle(color: AppColors.accent, fontSize: 13),
                            ),
                          ),
                        ],
                      ),
                    ),
                    Expanded(
                      child: _allContacts.isEmpty
                          ? const Center(
                              child: Text('No contacts with a phone number', style: TextStyle(color: AppColors.textDim)))
                          : ListView.builder(
                              itemCount: visible.length,
                              itemBuilder: (context, i) {
                                final index = visible[i];
                                final contact = _allContacts[index];
                                final selected = _selected.contains(index);
                                void toggle() => setState(() {
                                      if (selected) {
                                        _selected.remove(index);
                                      } else {
                                        _selected.add(index);
                                      }
                                    });
                                return ListTile(
                                  leading: CircleAvatar(
                                    backgroundColor: AppColors.accent.withValues(alpha: selected ? 0.3 : 0.1),
                                    child: Text(
                                      _initials(contact.name),
                                      style: TextStyle(
                                        color: selected ? AppColors.accent : AppColors.textDim,
                                        fontSize: 14,
                                        fontWeight: FontWeight.w600,
                                      ),
                                    ),
                                  ),
                                  title: Text(contact.name.isEmpty ? contact.phone : contact.name,
                                      style: const TextStyle(color: AppColors.text, fontSize: 14)),
                                  subtitle: Text(contact.phone,
                                      style: const TextStyle(color: AppColors.textDim, fontSize: 12)),
                                  trailing: Checkbox(
                                    value: selected,
                                    onChanged: (_) => toggle(),
                                    activeColor: AppColors.accent,
                                    side: const BorderSide(color: AppColors.textDim),
                                  ),
                                  onTap: toggle,
                                );
                              },
                            ),
                    ),
                  ],
                ),
    );
  }
}
