// Scoped LaunchServices launch and read-only compositing gate. Activates only when the launch spec sets activates=true.
import AppKit
import CoreGraphics
// Screen frames in global CoreGraphics coordinates (origin at the top left of the main display), the space of CGWindow bounds and CGEvent locations.
func cgFrame(_ r: NSRect) -> CGRect { CGRect(x: r.minX, y: (NSScreen.screens.first?.frame.height ?? 0) - r.maxY, width: r.width, height: r.height) }
import Foundation
func emit(_ value: [String: Any]) {
    let data = try! JSONSerialization.data(withJSONObject: value, options: [.sortedKeys])
    print(String(data: data, encoding: .utf8)!); fflush(stdout)
}
let args = Array(CommandLine.arguments.dropFirst())
if args.first == "launch" {
    let spec = try! JSONSerialization.jsonObject(with: Data(contentsOf: URL(fileURLWithPath: args[1]))) as! [String: Any]
    let config = NSWorkspace.OpenConfiguration()
    // Activation at launch only when the spec says so (authorized own-window foreground runs); otherwise never activates.
    config.activates = spec["activates"] as? Bool ?? false
    config.hides = false
    config.addsToRecentItems = false
    config.createsNewApplicationInstance = true
    config.arguments = spec["arguments"] as! [String]
    config.environment = spec["environment"] as! [String: String]
    NSWorkspace.shared.openApplication(at: URL(fileURLWithPath: spec["bundle"] as! String), configuration: config) { app, error in
        if let app { emit(["pid": Int(app.processIdentifier), "passive": true]); exit(0) }
        emit(["error": error?.localizedDescription ?? "LaunchServices returned no application"]); exit(2)
    }
    RunLoop.main.run()
} else if args.first == "gate", let pid = Int32(args[1]) {
    var result: [String: Any] = ["pid": Int(pid)]
    guard let app = NSRunningApplication(processIdentifier: pid) else { emit(["pid": Int(pid), "error": "own process missing"]); exit(3) }
    result["hidden"] = app.isHidden
    result["active"] = app.isActive
    let list = CGWindowListCopyWindowInfo([.optionOnScreenOnly, .excludeDesktopElements], kCGNullWindowID) as? [[String: Any]] ?? []
    func frame(_ w: [String: Any]) -> CGRect? {
        guard let b = w[kCGWindowBounds as String] as? [String: Double], let x = b["X"], let y = b["Y"], let width = b["Width"], let height = b["Height"] else { return nil }
        return CGRect(x: x, y: y, width: width, height: height)
    }
    let own = list.enumerated().filter { ($0.element[kCGWindowOwnerPID as String] as? Int32) == pid && ($0.element[kCGWindowLayer as String] as? Int) == 0 }
    guard let target = own.max(by: { (frame($0.element)?.width ?? 0)*(frame($0.element)?.height ?? 0) < (frame($1.element)?.width ?? 0)*(frame($1.element)?.height ?? 0) }), let rect = frame(target.element) else {
        result["visible"] = false; result["error"] = "own normal window not on screen"; emit(result); exit(3)
    }
    result["visible"] = true
    result["window"] = target.element[kCGWindowNumber as String]
    result["bounds"] = ["x": rect.minX, "y": rect.minY, "w": rect.width, "h": rect.height]
    // The Dock keeps a full-screen layer-20 window above every normal window; events pass through it (same rule as qa_guard).
    func dockLayer(_ w: [String: Any], _ f: CGRect) -> Bool {
        guard (w[kCGWindowLayer as String] as? Int) == 20, NSScreen.screens.contains(where: { cgFrame($0.frame) == f }), let p = w[kCGWindowOwnerPID as String] as? Int32 else { return false }
        return NSRunningApplication(processIdentifier: p)?.bundleIdentifier == "com.apple.dock"
    }
    // Foreign windows are reduced to layer and stacking order; no owner, title or pixels are emitted or retained.
    let overlays = list.prefix(target.offset).enumerated().compactMap { (i, w) -> [String: Any]? in
        guard (w[kCGWindowOwnerPID as String] as? Int32) != pid, let other = frame(w), (w[kCGWindowAlpha as String] as? Double ?? 1) > 0 else { return nil }
        guard other.intersects(rect), !other.intersection(rect).isEmpty, !dockLayer(w, other) else { return nil }
        return ["order": i, "layer": w[kCGWindowLayer as String] ?? -1]
    }
    let covered = !overlays.isEmpty
    result["overlays"] = overlays
    let mainHeight = NSScreen.screens.first?.frame.height ?? 0
    let cocoaRect = CGRect(x: rect.minX, y: mainHeight-rect.maxY, width: rect.width, height: rect.height)
    let screen = NSScreen.screens.first { $0.visibleFrame.contains(cocoaRect) }
    result["withinVisibleFrame"] = screen != nil
    result["scale"] = screen?.backingScaleFactor ?? NSNull()
    result["foreignOverlay"] = covered
    result["exposure"] = !app.isHidden && screen != nil && !covered
    emit(result)
} else { emit(["error": "usage: passive_window launch SPEC | gate PID"]); exit(2) }
