//
//  VideoRecorder.swift
//  SmartMailboxApp
//
//  Records the live MJPEG stream to a video file, then saves it to the
//  Photos library - the iOS equivalent of tools/mailbox_gui.py's
//  cv2.VideoWriter-based recorder (which saves an .avi to a file the user
//  picks). Each decoded JPEG frame is drawn into a CVPixelBuffer and
//  appended to an AVAssetWriter at a nominal 10fps, matching the desktop
//  version's fixed playback rate (not a measurement of the camera's
//  actual frame rate - the source MJPEG stream's real frame arrival rate
//  varies with network conditions).
//

import AVFoundation
import Photos
import UIKit

final class VideoRecorder {
    private var writer: AVAssetWriter?
    private var input: AVAssetWriterInput?
    private var adaptor: AVAssetWriterInputPixelBufferAdaptor?
    private var outputURL: URL?
    private var frameCount: Int64 = 0
    private let fps: Int32 = 10

    func start() {
        let url = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString + ".mov")
        outputURL = url
        writer = try? AVAssetWriter(outputURL: url, fileType: .mov)
        frameCount = 0
        // input/adaptor are created lazily on the first frame, once we
        // know its actual pixel dimensions - see append(image:).
    }

    /// Feed one decoded video frame in. Safe to call even if recording
    /// hasn't been start()ed - AppState only calls this while isRecording
    /// is true, but this stays a no-op either way if writer is nil.
    func append(image: UIImage) {
        guard let writer = writer, let cgImage = image.cgImage else { return }

        if input == nil {
            let width = cgImage.width
            let height = cgImage.height
            let outputSettings: [String: Any] = [
                AVVideoCodecKey: AVVideoCodecType.h264,
                AVVideoWidthKey: width,
                AVVideoHeightKey: height,
            ]
            let newInput = AVAssetWriterInput(mediaType: .video, outputSettings: outputSettings)
            newInput.expectsMediaDataInRealTime = true
            let pixelBufferAttributes: [String: Any] = [
                kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_32ARGB,
                kCVPixelBufferWidthKey as String: width,
                kCVPixelBufferHeightKey as String: height,
            ]
            let newAdaptor = AVAssetWriterInputPixelBufferAdaptor(
                assetWriterInput: newInput, sourcePixelBufferAttributes: pixelBufferAttributes
            )
            guard writer.canAdd(newInput) else { return }
            writer.add(newInput)
            writer.startWriting()
            writer.startSession(atSourceTime: .zero)
            input = newInput
            adaptor = newAdaptor
        }

        guard let adaptor = adaptor, let pool = adaptor.pixelBufferPool,
              input?.isReadyForMoreMediaData == true else { return }

        var pixelBufferOut: CVPixelBuffer?
        CVPixelBufferPoolCreatePixelBuffer(nil, pool, &pixelBufferOut)
        guard let pixelBuffer = pixelBufferOut else { return }

        CVPixelBufferLockBaseAddress(pixelBuffer, [])
        let context = CGContext(
            data: CVPixelBufferGetBaseAddress(pixelBuffer),
            width: cgImage.width,
            height: cgImage.height,
            bitsPerComponent: 8,
            bytesPerRow: CVPixelBufferGetBytesPerRow(pixelBuffer),
            space: CGColorSpaceCreateDeviceRGB(),
            bitmapInfo: CGImageAlphaInfo.noneSkipFirst.rawValue
        )
        context?.draw(cgImage, in: CGRect(x: 0, y: 0, width: cgImage.width, height: cgImage.height))
        CVPixelBufferUnlockBaseAddress(pixelBuffer, [])

        let time = CMTime(value: frameCount, timescale: fps)
        adaptor.append(pixelBuffer, withPresentationTime: time)
        frameCount += 1
    }

    /// Finishes writing and saves the result to the Photos library.
    /// Always clears internal state afterward, even on failure, so a
    /// subsequent start() begins cleanly.
    func stop(completion: @escaping (Result<Void, Error>) -> Void) {
        guard let writer = writer, let input = input, let url = outputURL else {
            self.writer = nil
            self.input = nil
            self.adaptor = nil
            self.outputURL = nil
            completion(.failure(RecorderError.nothingRecorded))
            return
        }
        input.markAsFinished()
        writer.finishWriting { [weak self] in
            self?.saveToPhotos(url: url, completion: completion)
            self?.writer = nil
            self?.input = nil
            self?.adaptor = nil
            self?.outputURL = nil
        }
    }

    private func saveToPhotos(url: URL, completion: @escaping (Result<Void, Error>) -> Void) {
        PHPhotoLibrary.requestAuthorization(for: .addOnly) { status in
            guard status == .authorized || status == .limited else {
                DispatchQueue.main.async { completion(.failure(RecorderError.photosPermissionDenied)) }
                return
            }
            PHPhotoLibrary.shared().performChanges({
                PHAssetChangeRequest.creationRequestForAssetFromVideo(atFileURL: url)
            }, completionHandler: { success, error in
                DispatchQueue.main.async {
                    if success {
                        completion(.success(()))
                    } else {
                        completion(.failure(error ?? RecorderError.unknownSaveError))
                    }
                }
            })
        }
    }

    enum RecorderError: LocalizedError {
        case nothingRecorded
        case photosPermissionDenied
        case unknownSaveError

        var errorDescription: String? {
            switch self {
            case .nothingRecorded: return "No frames were recorded"
            case .photosPermissionDenied: return "Photos permission denied"
            case .unknownSaveError: return "Unknown error saving to Photos"
            }
        }
    }
}
