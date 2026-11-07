// qa_place PID X Y W H: moves the largest AX window of a QA app. Only the app under test is ever addressed.
import ApplicationServices
import Foundation
let a = CommandLine.arguments.dropFirst().map { Double($0)! }
let app = AXUIElementCreateApplication(pid_t(a[0]))
var windows: CFTypeRef?
guard AXUIElementCopyAttributeValue(app, kAXWindowsAttribute as CFString, &windows) == .success, let list = windows as? [AXUIElement], let window = list.first else { print("no window"); exit(3) }
var position = CGPoint(x: a[1], y: a[2]), size = CGSize(width: a[3], height: a[4])
AXUIElementSetAttributeValue(window, kAXPositionAttribute as CFString, AXValueCreate(.cgPoint, &position)!)
AXUIElementSetAttributeValue(window, kAXSizeAttribute as CFString, AXValueCreate(.cgSize, &size)!)
AXUIElementSetAttributeValue(window, kAXPositionAttribute as CFString, AXValueCreate(.cgPoint, &position)!)
print("ok")
