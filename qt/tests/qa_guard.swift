// qa_guard PID [X Y] [--window ID] [--allow PID]...: exit 0 only if a synthetic event at this point can reach PID.
// Checks the foreground application, that the target window still exists, and the top window under the point.
// The app's own windows (menus, dialogs) and an explicitly trusted helper pass; every other owner is refused.
import AppKit
import CoreGraphics
// Screen frames in global CoreGraphics coordinates (origin at the top left of the main display), the space of CGWindow bounds and CGEvent locations.
func cgFrame(_ r: NSRect) -> CGRect { CGRect(x: r.minX, y: (NSScreen.screens.first?.frame.height ?? 0) - r.maxY, width: r.width, height: r.height) }
func refuse(_ reason: String) -> Never { print("refuse \(reason)"); exit(3) }
var arguments = Array(CommandLine.arguments.dropFirst())
var allow = Set<Int>(), windowID: Int? = nil
while let i = arguments.firstIndex(of: "--allow") { allow.insert(Int(arguments[i + 1])!); arguments.removeSubrange(i...(i + 1)) }
if let i = arguments.firstIndex(of: "--window") { windowID = Int(arguments[i + 1]); arguments.removeSubrange(i...(i + 1)) }
let pid = Int(arguments[0])!
var point: CGPoint? = arguments.count >= 3 ? CGPoint(x: Double(arguments[1])!, y: Double(arguments[2])!) : nil
guard let front = NSWorkspace.shared.frontmostApplication else { refuse("no foreground application") }
if Int(front.processIdentifier) != pid { refuse("foreground is not the owned PID") }
let list = CGWindowListCopyWindowInfo([.optionOnScreenOnly, .excludeDesktopElements], kCGNullWindowID) as! [[String: Any]]
func owner(_ w: [String: Any]) -> Int { w[kCGWindowOwnerPID as String] as? Int ?? -1 }
if let windowID, !list.contains(where: { ($0[kCGWindowNumber as String] as? Int) == windowID && owner($0) == pid }) { refuse("target window \(windowID) is not on screen") }
guard let point else { print("ok foreground"); exit(0) }
// The point must lie in the visibleFrame (no menu bar, no Dock) of the display that contains it.
guard let screen = NSScreen.screens.first(where: { cgFrame($0.frame).contains(point) }) else { refuse("no screen at the point") }
if !cgFrame(screen.visibleFrame).contains(point) { refuse("point outside the visible frame") }
for w in list {
  guard let b = w[kCGWindowBounds as String] as? [String: Double] else { continue }
  let frame = CGRect(x: b["X"]!, y: b["Y"]!, width: b["Width"]!, height: b["Height"]!)
  if !frame.contains(point) { continue }
  let name = w[kCGWindowOwnerName as String] as? String ?? "?"
  let layer = w[kCGWindowLayer as String] as? Int ?? 0
  if owner(w) == pid { print("ok own window"); exit(0) }
  if allow.contains(owner(w)) { print("ok trusted helper"); exit(0) }
  // The Dock keeps a transparent full-screen layer; its real area is outside visibleFrame, checked above.
  if name == "Dock" && layer == 20 && NSScreen.screens.contains(where: { cgFrame($0.frame) == frame }) { continue }
  refuse("foreign overlay is above the target")
}
refuse("no window under the point")
