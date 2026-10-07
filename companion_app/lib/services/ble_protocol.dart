import 'dart:convert';
import 'dart:typed_data';
import 'package:flutter_blue_plus/flutter_blue_plus.dart';

class BleProtocol {
  static final serviceUuid = Guid('19B10000-E8F2-537E-4F6C-D104768A1214');
  static final batteryUuid = Guid('19B10001-E8F2-537E-4F6C-D104768A1214');
  static final notifUuid = Guid('19B10002-E8F2-537E-4F6C-D104768A1214');
  static final timeUuid = Guid('19B10003-E8F2-537E-4F6C-D104768A1214');
  static final contactsUuid = Guid('19B10004-E8F2-537E-4F6C-D104768A1214');
  static final mediaUuid = Guid('19B10005-E8F2-537E-4F6C-D104768A1214');
  static final fileUuid = Guid('19B10006-E8F2-537E-4F6C-D104768A1214');
  static final notesUuid = Guid('19B10007-E8F2-537E-4F6C-D104768A1214');
  static final controllerUuid = Guid('19B10008-E8F2-537E-4F6C-D104768A1214');
  /// Phone link: JSON messages both ways (calls, media, notifications with
  /// actions, find my phone, phone battery, watch steps). See
  /// COMPANION_PROTOCOL.md in the firmware folder.
  static final linkUuid = Guid('19B10009-E8F2-537E-4F6C-D104768A1214');
  /// Firmware update (binary protocol, see COMPANION_PROTOCOL.md).
  static final otaUuid = Guid('19B1000A-E8F2-537E-4F6C-D104768A1214');

  static const deviceName = 'AmoledWatch';
  static const maxNotifLen = 128;
  static const maxContactsLen = 8191; // watch buffer (8 KB incl. terminator)
  static const maxTranscriptLen = 8191;
  static const maxMediaLen = 256;
  static const maxFileLen = 4096;

  // "Find My Watch" - a reserved sentinel written to the notification
  // characteristic instead of a real notification. The firmware's
  // NotifWriteCallback checks for this exact string first (see
  // hal_ble.cpp) and triggers a full-screen pulse + repeating vibrate/
  // beep pattern instead of queueing it as a normal notification -
  // must match BLE_FINDME_SENTINEL there exactly.
  static const findMeSentinel = '\x01__FINDME__\x01';

  // D-pad and button bit values must match hal_controller.h on the
  // firmware side exactly - up/down/left/right = 0x01/0x02/0x04/0x08,
  // same numbering for the 4 action buttons (A/B/X/Y).
  static Uint8List encodeController({
    bool up = false, bool down = false, bool left = false, bool right = false,
    bool a = false, bool b = false, bool x = false, bool y = false,
  }) {
    int dpad = (up ? 0x01 : 0) | (down ? 0x02 : 0) | (left ? 0x04 : 0) | (right ? 0x08 : 0);
    int buttons = (a ? 0x01 : 0) | (b ? 0x02 : 0) | (x ? 0x04 : 0) | (y ? 0x08 : 0);
    return Uint8List.fromList([dpad, buttons]);
  }

  static Uint8List encodeTime(DateTime dt) {
    final data = Uint8List(7);
    data[0] = (dt.year - 2000) & 0xFF;
    data[1] = dt.month & 0xFF;
    data[2] = dt.day & 0xFF;
    data[3] = dt.hour & 0xFF;
    data[4] = dt.minute & 0xFF;
    data[5] = dt.second & 0xFF;
    data[6] = 0;
    return data;
  }

  /// UTF-8 bytes of [text], cut to at most [maxBytes] without splitting a
  /// multi-byte character (a split one shows up as garbage on the watch).
  static Uint8List utf8Truncate(String text, int maxBytes) {
    final bytes = utf8.encode(text);
    if (bytes.length <= maxBytes) return Uint8List.fromList(bytes);
    var end = maxBytes;
    while (end > 0 && (bytes[end] & 0xC0) == 0x80) {
      end--; // bytes[end] is a continuation byte: back up to a char start
    }
    return Uint8List.fromList(bytes.sublist(0, end));
  }

  // The watch keeps 127 bytes + terminator.
  static Uint8List encodeNotification(String text) => utf8Truncate(text, maxNotifLen - 1);

  /// How many contacts from the start of [contacts] fit in the watch's
  /// buffer as a JSON array.
  static int contactsThatFit(List<ContactEntry> contacts) {
    var size = 2, n = 0; // "[]"
    for (final c in contacts) {
      final len = utf8.encode(jsonEncode(c.toJson())).length + (n > 0 ? 1 : 0);
      if (size + len > maxContactsLen) break;
      size += len;
      n++;
    }
    return n;
  }

  /// Contacts as a JSON array. If the list is too big for the watch,
  /// whole contacts are dropped from the end so the JSON stays valid.
  static Uint8List encodeContacts(List<ContactEntry> contacts) => Uint8List.fromList(utf8.encode(
      jsonEncode(contacts.take(contactsThatFit(contacts)).map((c) => c.toJson()).toList())));

  /// Splits [data] into packets for a characteristic that takes at most
  /// [maxPayload] bytes per write: first packet = [startCmd] + total
  /// length (2 bytes LE) + data, then [contCmd] + data. Used for contacts
  /// (0x01/0x02) and transcripts (0x06/0x07) - see hal_ble.cpp.
  static List<Uint8List> chunkPackets(Uint8List data, int startCmd, int contCmd, int maxPayload) {
    final packets = <Uint8List>[];
    var offset = 0;
    var first = true;
    do {
      final room = maxPayload - (first ? 3 : 1);
      final end = (offset + room).clamp(0, data.length);
      final chunk = data.sublist(offset, end);
      final p = BytesBuilder();
      if (first) {
        p.add([startCmd, data.length & 0xFF, (data.length >> 8) & 0xFF]);
      } else {
        p.addByte(contCmd);
      }
      p.add(chunk);
      packets.add(p.toBytes());
      offset = end;
      first = false;
    } while (offset < data.length);
    return packets;
  }

  static Uint8List encodeMedia(MediaState media) {
    final str = jsonEncode(media.toJson());
    final bytes = utf8.encode(str);
    if (bytes.length > maxMediaLen) {
      return Uint8List.fromList(bytes.sublist(0, maxMediaLen));
    }
    return Uint8List.fromList(bytes);
  }

  static Uint8List encodeWeather(WeatherData weather) {
    final jsonBytes = utf8.encode(jsonEncode(weather.toJson()));
    final packet = Uint8List(1 + jsonBytes.length);
    packet[0] = 0x02; // weather prefix
    packet.setRange(1, 1 + jsonBytes.length, jsonBytes);
    return packet;
  }

  static Uint8List encodeFitness(FitnessData fitness) {
    final jsonBytes = utf8.encode(jsonEncode(fitness.toJson()));
    final packet = Uint8List(1 + jsonBytes.length);
    packet[0] = 0x03; // fitness prefix
    packet.setRange(1, 1 + jsonBytes.length, jsonBytes);
    return packet;
  }

  static Uint8List encodeWatchface(WatchfaceLayout layout) {
    final str = jsonEncode(layout.toJson());
    final bytes = utf8.encode(str);
    if (bytes.length > maxFileLen) {
      return Uint8List.fromList(bytes.sublist(0, maxFileLen));
    }
    return Uint8List.fromList(bytes);
  }
}

class ContactEntry {
  final String name;
  final String phone;
  final String email;

  ContactEntry({required this.name, this.phone = '', this.email = ''});

  Map<String, dynamic> toJson() => {'name': name, 'phone': phone, 'email': email};

  factory ContactEntry.fromJson(Map<String, dynamic> json) => ContactEntry(
    name: json['name'] ?? '',
    phone: json['phone'] ?? '',
    email: json['email'] ?? '',
  );
}

class MediaState {
  final String artist;
  final String album;
  final String title;
  final bool playing;
  final int position;
  final int duration;

  MediaState({
    this.artist = '',
    this.album = '',
    this.title = '',
    this.playing = false,
    this.position = 0,
    this.duration = 0,
  });

  Map<String, dynamic> toJson() => {
    'artist': artist,
    'album': album,
    'title': title,
    'playing': playing,
    'position': position,
    'duration': duration,
  };
}

class WeatherData {
  final double tempC;
  final String condition;
  final int humidity;
  final List<WeatherForecast> forecast;

  WeatherData({
    this.tempC = 0,
    this.condition = 'Unknown',
    this.humidity = 0,
    this.forecast = const [],
  });

  Map<String, dynamic> toJson() => {
    'tempC': tempC,
    'condition': condition,
    'humidity': humidity,
    'forecast': forecast.map((f) => f.toJson()).toList(),
  };
}

class WeatherForecast {
  final String day;
  final double high;
  final double low;
  final String condition;

  WeatherForecast({required this.day, required this.high, required this.low, required this.condition});

  Map<String, dynamic> toJson() => {'day': day, 'high': high, 'low': low, 'cond': condition};
}

class FitnessData {
  final int steps;
  final int calories;
  final double distanceKm;
  final int heartRate;

  FitnessData({this.steps = 0, this.calories = 0, this.distanceKm = 0, this.heartRate = 0});

  Map<String, dynamic> toJson() => {
    'steps': steps,
    'calories': calories,
    'distanceKm': distanceKm,
    'heartRate': heartRate,
  };
}

class WatchfaceLayout {
  final String bgColor;
  final List<WatchfaceWidget> widgets;

  WatchfaceLayout({this.bgColor = '#000000', List<WatchfaceWidget>? widgets})
      : widgets = widgets ?? [];

  Map<String, dynamic> toJson() => {
    'bg': bgColor,
    'widgets': widgets.map((w) => w.toJson()).toList(),
  };

  factory WatchfaceLayout.fromJson(Map<String, dynamic> json) => WatchfaceLayout(
    bgColor: json['bg'] ?? '#000000',
    widgets: (json['widgets'] as List? ?? []).map((w) => WatchfaceWidget.fromJson(w)).toList(),
  );

  String encode() => jsonEncode(toJson());
}

class WatchfaceWidget {
  final String type;
  double x;
  double y;
  double radius;
  String color;
  String font;
  double fontSize;
  String text;
  String dataSrc;

  WatchfaceWidget({
    required this.type,
    this.x = 233,
    this.y = 233,
    this.radius = 100,
    this.color = '#7B61FF',
    this.font = 'montserrat',
    this.fontSize = 14,
    this.text = '',
    this.dataSrc = '',
  });

  Map<String, dynamic> toJson() => {
    'type': type,
    'x': x,
    'y': y,
    'radius': radius,
    'color': color,
    'font': font,
    'fontSize': fontSize,
    'text': text,
    'dataSrc': dataSrc,
  };

  factory WatchfaceWidget.fromJson(Map<String, dynamic> json) => WatchfaceWidget(
    type: json['type'] ?? 'text',
    x: (json['x'] ?? 233).toDouble(),
    y: (json['y'] ?? 233).toDouble(),
    radius: (json['radius'] ?? 100).toDouble(),
    color: json['color'] ?? '#7B61FF',
    font: json['font'] ?? 'montserrat',
    fontSize: (json['fontSize'] ?? 14).toDouble(),
    text: json['text'] ?? '',
    dataSrc: json['dataSrc'] ?? '',
  );
}

class RecordingEntry {
  final String name;
  final int size;
  String transcript;
  String summary;
  int importance; // 1-5
  List<String> tags;
  DateTime? recordedAt;
  // Knowledge fields (filled by the summary model, or locally as a fallback)
  String title;           // short human title, '' = use the file name
  List<String> topics;    // 2-4 short topic phrases
  List<String> actions;   // things to do mentioned in the memo
  List<String> people;    // names mentioned
  int durationSec;        // 0 = unknown
  bool favorite;
  DateTime? processedAt;  // when transcript + summary were made
  String language;        // ISO code of the speech ('' = unknown)
  bool languageSet;       // chosen by the user (else detected)
  bool noSpeech;          // transcription found no speech: skipped by "Process everything"

  RecordingEntry({
    required this.name,
    this.size = 0,
    this.transcript = '',
    this.summary = '',
    this.importance = 3,
    this.tags = const [],
    this.recordedAt,
    this.title = '',
    this.topics = const [],
    this.actions = const [],
    this.people = const [],
    this.durationSec = 0,
    this.favorite = false,
    this.processedAt,
    this.language = '',
    this.languageSet = false,
    this.noSpeech = false,
  });

  /// Title to show: the AI title, else a cleaned-up file name.
  String get displayTitle {
    if (title.trim().isNotEmpty) return title.trim();
    final n = name.replaceAll(RegExp(r'\.(wav|mp3)$', caseSensitive: false), '').replaceAll('_', ' ');
    return n.isEmpty ? 'Memo' : n[0].toUpperCase() + n.substring(1);
  }

  bool get isProcessed => transcript.isNotEmpty && summary.isNotEmpty;

  RecordingEntry copyWith({
    int? size,
    String? transcript,
    String? summary,
    int? importance,
    List<String>? tags,
    DateTime? recordedAt,
    String? title,
    List<String>? topics,
    List<String>? actions,
    List<String>? people,
    int? durationSec,
    bool? favorite,
    DateTime? processedAt,
    String? language,
    bool? languageSet,
    bool? noSpeech,
  }) =>
      RecordingEntry(
        name: name,
        size: size ?? this.size,
        transcript: transcript ?? this.transcript,
        summary: summary ?? this.summary,
        importance: importance ?? this.importance,
        tags: tags ?? this.tags,
        recordedAt: recordedAt ?? this.recordedAt,
        title: title ?? this.title,
        topics: topics ?? this.topics,
        actions: actions ?? this.actions,
        people: people ?? this.people,
        durationSec: durationSec ?? this.durationSec,
        favorite: favorite ?? this.favorite,
        processedAt: processedAt ?? this.processedAt,
        language: language ?? this.language,
        languageSet: languageSet ?? this.languageSet,
        noSpeech: noSpeech ?? this.noSpeech,
      );

  Map<String, dynamic> toJson() => {
    'name': name,
    'size': size,
    'transcript': transcript,
    'summary': summary,
    'importance': importance,
    'tags': tags,
    if (recordedAt != null) 'recordedAt': recordedAt!.toIso8601String(),
    if (title.isNotEmpty) 'title': title,
    if (topics.isNotEmpty) 'topics': topics,
    if (actions.isNotEmpty) 'actions': actions,
    if (people.isNotEmpty) 'people': people,
    if (durationSec > 0) 'durationSec': durationSec,
    if (favorite) 'favorite': true,
    if (processedAt != null) 'processedAt': processedAt!.toIso8601String(),
    if (language.isNotEmpty) 'language': language,
    if (languageSet) 'languageSet': true,
    if (noSpeech) 'noSpeech': true,
  };

  static List<String> _strings(dynamic v) => (v as List<dynamic>?)?.map((e) => e.toString()).toList() ?? [];

  factory RecordingEntry.fromJson(Map<String, dynamic> json) => RecordingEntry(
    name: json['name'] ?? '',
    size: json['size'] ?? 0,
    transcript: json['transcript'] ?? '',
    summary: json['summary'] ?? '',
    importance: json['importance'] ?? 3,
    tags: _strings(json['tags']),
    recordedAt: json['recordedAt'] != null ? DateTime.tryParse(json['recordedAt']) : null,
    title: json['title'] ?? '',
    topics: _strings(json['topics']),
    actions: _strings(json['actions']),
    people: _strings(json['people']),
    durationSec: (json['durationSec'] as num?)?.toInt() ?? 0,
    favorite: json['favorite'] == true,
    processedAt: json['processedAt'] != null ? DateTime.tryParse(json['processedAt']) : null,
    language: json['language'] ?? '',
    languageSet: json['languageSet'] == true,
    noSpeech: json['noSpeech'] == true,
  );
}
