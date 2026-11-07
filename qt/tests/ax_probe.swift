// Assistive client probe: walks the app, reads every attribute incl. AXSelectedChildren, hit-tests, optionally presses a row.
// Usage: ax_probe PID [ROW_TITLE_TO_PRESS]. Prints one line per element: depth role title value.
import ApplicationServices
import Foundation
let pid = pid_t(CommandLine.arguments[1])!
let pressTitle = CommandLine.arguments.count > 2 ? CommandLine.arguments[2] : nil
let app = AXUIElementCreateApplication(pid)
func attribute(_ e: AXUIElement, _ name: String) -> CFTypeRef? {
  var v: CFTypeRef?
  AXUIElementCopyAttributeValue(e, name as CFString, &v)
  return v
}
var pressed = false
func walk(_ e: AXUIElement, _ depth: Int) {
  var names: CFArray?
  AXUIElementCopyAttributeNames(e, &names)
  for n in (names as? [String]) ?? [] { _ = attribute(e, n) }
  let role = attribute(e, kAXRoleAttribute) as? String ?? "?"
  let title = (attribute(e, kAXTitleAttribute) as? String) ?? (attribute(e, kAXDescriptionAttribute) as? String) ?? ""
  let selected = attribute(e, kAXSelectedAttribute) as? Bool ?? false
  print(String(repeating: " ", count: depth) + "\(role) '\(title)'\(selected ? " selected" : "")")
  if role == "AXList" || role == "AXOutline" {
    let chosen = attribute(e, kAXSelectedChildrenAttribute) as? [AXUIElement]
    print(String(repeating: " ", count: depth) + "  selectedChildren=\(chosen?.count ?? -1)")
  }
  if let target = pressTitle, !pressed, title == target, role != "AXWindow", role != "AXApplication" {
    pressed = true
    let status = AXUIElementPerformAction(e, kAXPressAction as CFString)
    print("pressed \(title) status=\(status.rawValue)")
  }
  if depth < 9, let kids = attribute(e, kAXChildrenAttribute) as? [AXUIElement] {
    for k in kids.prefix(120) { walk(k, depth + 1) }
  }
}
walk(app, 0)
var hits = 0
for y in stride(from: 70, to: 770, by: 50) {
  for x in stride(from: 40, to: 1300, by: 80) {
    var hit: AXUIElement?
    if AXUIElementCopyElementAtPosition(app, Float(x), Float(y), &hit) == .success, let h = hit {
      hits += 1
      walk(h, 12)
    }
  }
}
print("hits \(hits)")
print("done")
