import Flutter
import UIKit
import Vision

/// "com.amoledwatch.segment" "mask" {path}: the photo's subject for photo
/// watch faces (lib/services/photo_face.dart) - Apple's Vision foreground
/// mask (iOS 17+), the iPhone twin of ML Kit subject segmentation on
/// Android. Answers one byte per pixel of the image, 0 background .. 255
/// subject, or nil when there's none (or iOS 16).
final class Segmenter {
    init(messenger: FlutterBinaryMessenger) {
        FlutterMethodChannel(name: "com.amoledwatch.segment", binaryMessenger: messenger)
            .setMethodCallHandler { call, result in
                guard call.method == "mask", let path = (call.arguments as? [String: Any])?["path"] as? String else {
                    result(FlutterMethodNotImplemented)
                    return
                }
                DispatchQueue.global(qos: .userInitiated).async {
                    let mask = Segmenter.mask(path)
                    DispatchQueue.main.async {
                        result(mask.map { FlutterStandardTypedData(bytes: $0) })
                    }
                }
            }
    }

    private static func mask(_ path: String) -> Data? {
        guard #available(iOS 17.0, *), let img = UIImage(contentsOfFile: path)?.cgImage else { return nil }
        let req = VNGenerateForegroundInstanceMaskRequest()
        let handler = VNImageRequestHandler(cgImage: img, options: [:])
        do {
            try handler.perform([req])
            guard let obs = req.results?.first, !obs.allInstances.isEmpty else { return nil }
            // The mask at the image's own size, 0..1 per pixel
            let pb = try obs.generateScaledMaskForImage(forInstances: obs.allInstances, from: handler)
            CVPixelBufferLockBaseAddress(pb, .readOnly)
            defer { CVPixelBufferUnlockBaseAddress(pb, .readOnly) }
            guard CVPixelBufferGetPixelFormatType(pb) == kCVPixelFormatType_OneComponent32Float,
                  let base = CVPixelBufferGetBaseAddress(pb) else { return nil }
            let w = CVPixelBufferGetWidth(pb), h = CVPixelBufferGetHeight(pb)
            let stride = CVPixelBufferGetBytesPerRow(pb)
            var out = Data(count: w * h)
            out.withUnsafeMutableBytes { (o: UnsafeMutableRawBufferPointer) in
                for y in 0..<h {
                    let row = (base + y * stride).assumingMemoryBound(to: Float.self)
                    for x in 0..<w { o[y * w + x] = UInt8(max(0, min(255, row[x] * 255))) }
                }
            }
            return out
        } catch {
            return nil
        }
    }
}
