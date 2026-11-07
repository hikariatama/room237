// qa_framegrab WINDOW_ID SECONDS OUT_DIR: ScreenCaptureKit frames of exactly one window, nothing else on the screen can enter them.
// Prints "ready" once the stream runs; OUT_DIR/frames.json holds [{file, ms}] in presentation time.
import CoreImage
import CoreMedia
import Foundation
import ScreenCaptureKit
final class Grab: NSObject, SCStreamOutput {
  let out: URL, context = CIContext(), space = CGColorSpace(name: CGColorSpace.sRGB)!
  var frames: [[String: Any]] = [], first: Double? = nil
  init(out: URL) { self.out = out }
  func stream(_ stream: SCStream, didOutputSampleBuffer sample: CMSampleBuffer, of type: SCStreamOutputType) {
    guard type == .screen, sample.isValid, let buffer = sample.imageBuffer else { return }
    let t = CMTimeGetSeconds(sample.presentationTimeStamp)
    if first == nil { first = t }
    let name = String(format: "%05d.jpg", frames.count)
    try? context.writeJPEGRepresentation(of: CIImage(cvPixelBuffer: buffer), to: out.appendingPathComponent(name), colorSpace: space, options: [kCGImageDestinationLossyCompressionQuality as CIImageRepresentationOption: 0.75])
    frames.append(["file": name, "ms": (t - first!) * 1000])
  }
}
let args = CommandLine.arguments
let out = URL(fileURLWithPath: args[3])
try FileManager.default.createDirectory(at: out, withIntermediateDirectories: true)
let id = CGWindowID(args[1])!, seconds = Double(args[2])!
Task {
  do {
    let content = try await SCShareableContent.excludingDesktopWindows(false, onScreenWindowsOnly: true)
    guard let window = content.windows.first(where: { $0.windowID == id }) else { print("window \(id) not found"); exit(4) }
    let config = SCStreamConfiguration()
    config.width = Int(window.frame.width); config.height = Int(window.frame.height)
    config.minimumFrameInterval = CMTime(value: 1, timescale: 60)
    config.queueDepth = 8; config.showsCursor = false
    let grab = Grab(out: out)
    let stream = SCStream(filter: SCContentFilter(desktopIndependentWindow: window), configuration: config, delegate: nil)
    try stream.addStreamOutput(grab, type: .screen, sampleHandlerQueue: DispatchQueue(label: "grab"))
    try await stream.startCapture()
    print("ready"); fflush(stdout)
    try await Task.sleep(nanoseconds: UInt64(seconds * 1e9))
    try await stream.stopCapture()
    let data = try JSONSerialization.data(withJSONObject: grab.frames)
    try data.write(to: out.appendingPathComponent("frames.json"))
    print("done \(grab.frames.count)"); fflush(stdout)
    exit(0)
  } catch { print("error \(error)"); exit(5) }
}
dispatchMain()
