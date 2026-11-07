// qa_winid PID [--panels]: number of the largest on-screen window of PID; with --panels, its smaller windows above that one, front first.
import CoreGraphics
import Foundation
let pid = Int(CommandLine.arguments[1])!
let list = CGWindowListCopyWindowInfo([.optionOnScreenOnly, .excludeDesktopElements], kCGNullWindowID) as! [[String: Any]]
var best = (0, 0.0, -1)
for (i, w) in list.enumerated() where (w[kCGWindowOwnerPID as String] as? Int) == pid {
  let b = w[kCGWindowBounds as String] as! [String: Double]
  if b["Width"]! * b["Height"]! > best.1 { best = (w[kCGWindowNumber as String] as! Int, b["Width"]! * b["Height"]!, i) }
}
if CommandLine.arguments.contains("--main-frame") {
  let b = list[best.2][kCGWindowBounds as String] as! [String: Double]
  print("\(b["X"]!) \(b["Y"]!) \(b["Width"]!) \(b["Height"]!)")
} else if CommandLine.arguments.contains("--panels") {
  for w in list[0..<max(0, best.2)] where (w[kCGWindowOwnerPID as String] as? Int) == pid {
    let b = w[kCGWindowBounds as String] as! [String: Double]
    print("\(w[kCGWindowNumber as String] as! Int)\t\(b["X"]!)\t\(b["Y"]!)\t\(b["Width"]!)\t\(b["Height"]!)")
  }
} else {
  print(best.0)
}
