import 'dart:convert';
import 'dart:io';
import 'dart:typed_data';

/// The latest GitHub release: one tag carries both the companion app (.apk)
/// and the watch firmware (.bin) - see .github/workflows/build.yml.
class ReleaseInfo {
  final String version; // tag without the leading "v"
  final String pageUrl;
  final String? apkUrl;
  final int apkSize;
  final String? firmwareUrl;
  final int firmwareSize;

  const ReleaseInfo({
    required this.version,
    required this.pageUrl,
    this.apkUrl,
    this.apkSize = 0,
    this.firmwareUrl,
    this.firmwareSize = 0,
  });
}

/// Checks this project's GitHub releases and downloads their files.
class UpdateService {
  static const repo = 'MikeCheek/CustomOS-ESP32-S3-1.75';
  static const firmwarePrefix = 'AmoledSmartWatchOS-';

  /// Null when the repository has no published release yet.
  static Future<ReleaseInfo?> fetchLatest() async {
    final client = HttpClient()..connectionTimeout = const Duration(seconds: 15);
    try {
      final req = await client.getUrl(Uri.parse('https://api.github.com/repos/$repo/releases/latest'));
      req.headers.set(HttpHeaders.acceptHeader, 'application/vnd.github+json');
      req.headers.set(HttpHeaders.userAgentHeader, 'AmoledWatch-companion');
      final res = await req.close().timeout(const Duration(seconds: 20));
      final body = await res.transform(utf8.decoder).join();
      if (res.statusCode == 404) return null;
      if (res.statusCode == 403 || res.statusCode == 429) {
        throw const HttpException('GitHub is busy - try again later');
      }
      if (res.statusCode != 200) throw HttpException('GitHub answered ${res.statusCode}');
      final j = jsonDecode(body) as Map<String, dynamic>;
      var tag = (j['tag_name'] as String?) ?? '';
      if (tag.startsWith('v') || tag.startsWith('V')) tag = tag.substring(1);
      String? apk, fwPlain, fwSigned;
      int apkSize = 0, plainSize = 0, signedSize = 0;
      for (final a in (j['assets'] as List? ?? const [])) {
        final name = (a['name'] as String?) ?? '';
        final url = a['browser_download_url'] as String?;
        final size = (a['size'] as num?)?.toInt() ?? 0;
        if (url == null) continue;
        if (name.endsWith('.apk')) {
          apk = url;
          apkSize = size;
        } else if (name.startsWith(firmwarePrefix) && name.endsWith('.signed.bin')) {
          fwSigned = url;
          signedSize = size;
        } else if (name.startsWith(firmwarePrefix) && name.endsWith('.bin')) {
          fwPlain = url;
          plainSize = size;
        }
      }
      // The signed image works on every watch (one without a key just
      // ignores the signature), so it's preferred when there is one.
      return ReleaseInfo(
        version: tag,
        pageUrl: (j['html_url'] as String?) ?? 'https://github.com/$repo/releases',
        apkUrl: apk,
        apkSize: apkSize,
        firmwareUrl: fwSigned ?? fwPlain,
        firmwareSize: fwSigned != null ? signedSize : plainSize,
      );
    } on SocketException {
      throw const HttpException('No internet connection');
    } finally {
      client.close(force: true);
    }
  }

  /// Downloads [url] (following GitHub's redirect to its CDN). [onProgress]
  /// gets (received, total); [cancel] returning true stops the download.
  static Future<Uint8List> download(
    String url, {
    void Function(int received, int total)? onProgress,
    bool Function()? cancel,
  }) async {
    final client = HttpClient()..connectionTimeout = const Duration(seconds: 15);
    try {
      final req = await client.getUrl(Uri.parse(url));
      req.headers.set(HttpHeaders.userAgentHeader, 'AmoledWatch-companion');
      final res = await req.close().timeout(const Duration(seconds: 30));
      if (res.statusCode != 200) throw HttpException('Download failed (${res.statusCode})');
      final total = res.contentLength;
      final out = BytesBuilder(copy: false);
      await for (final chunk in res.timeout(const Duration(seconds: 30))) {
        if (cancel?.call() == true) throw const HttpException('Cancelled');
        out.add(chunk);
        onProgress?.call(out.length, total);
      }
      if (total > 0 && out.length != total) throw const HttpException('Download incomplete');
      return out.takeBytes();
    } on SocketException {
      throw const HttpException('Connection lost');
    } finally {
      client.close(force: true);
    }
  }
}
