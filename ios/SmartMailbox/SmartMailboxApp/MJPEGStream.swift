//
//  MJPEGStream.swift
//  SmartMailboxApp
//
//  Pulls apart the board's multipart MJPEG stream frame by frame, using
//  the Content-Length header the firmware sends before each JPEG (see
//  stream_handler() in Smart_Mailbox.ino). This is a direct port of
//  tools/mailbox_gui.py's MJPEGReader: same buffering strategy, same
//  "resync to the next boundary marker" recovery when a header doesn't
//  parse. Runs via URLSessionDataDelegate instead of a Python thread -
//  URLSession already delivers didReceive/didComplete off the main
//  thread, so the app hops back to @MainActor itself when applying
//  results (see AppState).
//

import Foundation
import UIKit

final class MJPEGStream: NSObject {
    private let url: URL
    private let onFrame: (UIImage) -> Void
    private let onError: (String) -> Void

    private var session: URLSession?
    private var task: URLSessionDataTask?
    private var buffer = Data()

    init(url: URL, onFrame: @escaping (UIImage) -> Void, onError: @escaping (String) -> Void) {
        self.url = url
        self.onFrame = onFrame
        self.onError = onError
    }

    func start() {
        let config = URLSessionConfiguration.default
        config.timeoutIntervalForRequest = 15
        // Unlimited: this is a long-lived stream, not a one-shot download -
        // the default resource timeout would otherwise cut it off.
        config.timeoutIntervalForResource = 0
        let session = URLSession(configuration: config, delegate: self, delegateQueue: nil)
        self.session = session
        let task = session.dataTask(with: url)
        self.task = task
        task.resume()
    }

    func stop() {
        task?.cancel()
        session?.invalidateAndCancel()
        task = nil
        session = nil
        buffer.removeAll()
    }

    // Each frame in the stream is:
    //   "\r\n--frame\r\n" + "Content-Type: ...\r\nContent-Length: N\r\n\r\n" + <N JPEG bytes>
    // Buffer until a full header block (terminated by a blank line,
    // "\r\n\r\n") is visible, then wait for at least N more bytes for the
    // JPEG payload before slicing a frame out.
    private func drainFrames() {
        let headerTerminator = Data("\r\n\r\n".utf8)
        while true {
            guard let headerRange = buffer.range(of: headerTerminator) else { return }
            let header = buffer.subdata(in: buffer.startIndex..<headerRange.lowerBound)
            guard let headerText = String(data: header, encoding: .utf8),
                  let length = contentLength(from: headerText) else {
                // Not a valid frame header (e.g. still positioned
                // mid-boundary-marker from a previous partial read) -
                // resync to the next boundary marker and try again.
                if let boundary = buffer.range(of: Data("--frame".utf8)) {
                    buffer.removeSubrange(buffer.startIndex..<boundary.lowerBound)
                    continue
                } else {
                    buffer.removeAll()
                    return
                }
            }
            let frameStart = headerRange.upperBound
            let frameEnd = frameStart + length
            // Compare against endIndex, not count: Data's indices aren't
            // guaranteed to stay zero-based after removeSubrange, so count
            // (endIndex - startIndex) isn't safely comparable to an
            // absolute index like frameEnd.
            guard buffer.endIndex >= frameEnd else { return }  // full JPEG payload not buffered yet
            let frameData = buffer.subdata(in: frameStart..<frameEnd)
            buffer.removeSubrange(buffer.startIndex..<frameEnd)
            if let image = UIImage(data: frameData) {
                onFrame(image)
            }
        }
    }

    private func contentLength(from header: String) -> Int? {
        for line in header.components(separatedBy: "\r\n") {
            let lower = line.lowercased()
            if lower.hasPrefix("content-length:") {
                let valueText = line.dropFirst("content-length:".count).trimmingCharacters(in: .whitespaces)
                return Int(valueText)
            }
        }
        return nil
    }
}

extension MJPEGStream: URLSessionDataDelegate {
    func urlSession(_ session: URLSession, dataTask: URLSessionDataTask, didReceive data: Data) {
        buffer.append(data)
        drainFrames()
    }

    func urlSession(_ session: URLSession, task: URLSessionTask, didCompleteWithError error: Error?) {
        guard let error = error, (error as NSError).code != NSURLErrorCancelled else { return }
        onError("Stream error: \(error.localizedDescription)")
    }
}
