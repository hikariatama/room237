// A non-activating panel that starts a real NSDraggingSession with file URLs when it
// receives a mouse drag. Argument 1: text file with one absolute path per line.
import AppKit

final class Source: NSView, NSDraggingSource {
  var urls: [URL] = []
  var started = false
  func draggingSession(_ session: NSDraggingSession, sourceOperationMaskFor context: NSDraggingContext) -> NSDragOperation { .copy }
  override func acceptsFirstMouse(for event: NSEvent?) -> Bool { true }
  override func mouseDragged(with event: NSEvent) {
    guard !started else { return }
    started = true
    let items = urls.enumerated().map { (i, url) -> NSDraggingItem in
      let item = NSDraggingItem(pasteboardWriter: url as NSURL)
      item.setDraggingFrame(NSRect(x: 0, y: 0, width: 32, height: 32), contents: NSImage(size: NSSize(width: 32, height: 32)))
      return item
    }
    beginDraggingSession(with: items, event: event, source: self)
  }
  func draggingSession(_ session: NSDraggingSession, endedAt screenPoint: NSPoint, operation: NSDragOperation) {
    print("ended \(operation.rawValue)"); fflush(stdout)
    DispatchQueue.main.asyncAfter(deadline: .now() + 0.2) { NSApp.terminate(nil) }
  }
  override func draw(_ dirtyRect: NSRect) { NSColor.systemTeal.setFill(); dirtyRect.fill() }
}
let app = NSApplication.shared
app.setActivationPolicy(.accessory)
let paths = (try? String(contentsOfFile: CommandLine.arguments[1], encoding: .utf8))?.split(separator: "\n").map(String.init) ?? []
// ROOM237_DRAGSOURCE_AT="x,y": top-left of the panel in global CoreGraphics points (default: a fixed spot on the main display).
let at = ProcessInfo.processInfo.environment["ROOM237_DRAGSOURCE_AT"]?.split(separator: ",").compactMap { Double($0) }
let origin = at?.count == 2 ? NSPoint(x: at![0], y: NSScreen.screens[0].frame.height - at![1] - 90) : NSPoint(x: 4, y: 500)
let panel = NSPanel(contentRect: NSRect(origin: origin, size: NSSize(width: 90, height: 90)), styleMask: [.nonactivatingPanel, .borderless], backing: .buffered, defer: false)
panel.level = .floating
let view = Source(frame: NSRect(x: 0, y: 0, width: 90, height: 90))
view.urls = paths.map { URL(fileURLWithPath: $0) }
panel.contentView = view
panel.orderFrontRegardless()
print("ready \(Int(panel.frame.midX)) \(Int((NSScreen.screens[0].frame.height) - panel.frame.midY))"); fflush(stdout)
app.run()
