// qa_front PID: makes the QA app frontmost through Accessibility (AXFrontmost, then AXRaise of its largest window).
// Only the app under test is ever addressed; prints "ok active=<Bool>" or a refusal.
import AppKit
import ApplicationServices
let pid = pid_t(CommandLine.arguments[1])!
guard let running = NSRunningApplication(processIdentifier: pid) else { print("refuse no process"); exit(3) }
let app = AXUIElementCreateApplication(pid)
guard AXUIElementSetAttributeValue(app, kAXFrontmostAttribute as CFString, kCFBooleanTrue) == .success else { print("refuse AXFrontmost failed"); exit(3) }
var windows: CFTypeRef?
if AXUIElementCopyAttributeValue(app, kAXWindowsAttribute as CFString, &windows) == .success, let list = windows as? [AXUIElement] {
  func area(_ w: AXUIElement) -> Double {
    var v: CFTypeRef?; var s = CGSize.zero
    if AXUIElementCopyAttributeValue(w, kAXSizeAttribute as CFString, &v) == .success { AXValueGetValue(v as! AXValue, .cgSize, &s) }
    return s.width * s.height
  }
  if let main = list.max(by: { area($0) < area($1) }) { AXUIElementPerformAction(main, kAXRaiseAction as CFString) }
}
let deadline = Date().addingTimeInterval(2)
while !running.isActive && Date() < deadline { RunLoop.current.run(until: Date().addingTimeInterval(0.05)) }
print("ok active=\(running.isActive)")
