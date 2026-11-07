// qa_occluder X Y W H: a test-owned magenta panel used to prove that the input guard refuses. Prints "ready PID";
// reads "front" (take the foreground) or "quit" from stdin and reports every click it receives as "clicked".
import AppKit
let a = CommandLine.arguments.dropFirst().map { Double($0)! }
final class View: NSView {
  override func draw(_ r: NSRect) { NSColor(srgbRed: 1, green: 0, blue: 1, alpha: 1).setFill(); r.fill() }
  override func mouseDown(with event: NSEvent) { print("clicked"); fflush(stdout) }
  override func rightMouseDown(with event: NSEvent) { print("clicked"); fflush(stdout) }
  override func acceptsFirstMouse(for event: NSEvent?) -> Bool { true }
}
let app = NSApplication.shared
app.setActivationPolicy(.accessory)
let height = NSScreen.screens[0].frame.height
let panel = NSPanel(contentRect: NSRect(x: a[0], y: height - a[1] - a[3], width: a[2], height: a[3]), styleMask: [.nonactivatingPanel, .borderless], backing: .buffered, defer: false)
panel.level = .floating
panel.contentView = View(frame: NSRect(x: 0, y: 0, width: a[2], height: a[3]))
panel.orderFrontRegardless()
FileHandle.standardInput.readabilityHandler = { handle in
  let text = String(decoding: handle.availableData, as: UTF8.self)
  DispatchQueue.main.async {
    if text.contains("front") { app.activate(ignoringOtherApps: true); print("fronted"); fflush(stdout) }
    if text.contains("quit") || text.isEmpty { exit(0) }
  }
}
DispatchQueue.main.asyncAfter(deadline: .now() + 0.2) { print("ready \(getpid())"); fflush(stdout) }
app.run()
