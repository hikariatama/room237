// Trusted input helper: posts real CGEvents. Reads one JSON command per line and
// prints "ok ..." after finishing. Coordinates are global display points.
import AppKit
import CoreGraphics
// Screen frames in global CoreGraphics coordinates (origin at the top left of the main display), the space of CGWindow bounds and CGEvent locations.
func cgFrame(_ r: NSRect) -> CGRect { CGRect(x: r.minX, y: (NSScreen.screens.first?.frame.height ?? 0) - r.maxY, width: r.width, height: r.height) }
import Foundation

let source = CGEventSource(stateID: .hidSystemState)
func sleepMs(_ ms: Double) { if ms > 0 { usleep(UInt32(ms * 1000)) } }
var guardedPID: Int32 = 0
var guardedWindow: Int = 0
var guardedAllow = Set<Int32>()
// The Dock keeps a full-screen layer-20 window above every normal window; events pass through it (same rule as qa_guard and the gate).
func dockLayer(_ row: [String: Any], _ rect: CGRect) -> Bool {
  guard (row[kCGWindowLayer as String] as? Int) == 20, NSScreen.screens.contains(where: { cgFrame($0.frame) == rect }), let pid = row[kCGWindowOwnerPID as String] as? Int32 else { return false }
  return NSRunningApplication(processIdentifier: pid)?.bundleIdentifier == "com.apple.dock"
}
func checkEvent(_ event: CGEvent) {
  guard let expiryText = ProcessInfo.processInfo.environment["ROOM237_SLOT_EXPIRES_EPOCH"],
        let expiry = Double(expiryText), Date().timeIntervalSince1970 < expiry else { print("refuse slot expired or missing"); exit(3) }
  guard guardedPID > 0, guardedWindow > 0,
        NSWorkspace.shared.frontmostApplication?.processIdentifier == guardedPID else { print("refuse foreground or missing bound identity"); exit(3) }
  let list = CGWindowListCopyWindowInfo([.optionOnScreenOnly, .excludeDesktopElements], kCGNullWindowID) as? [[String: Any]] ?? []
  guard list.contains(where: { ($0[kCGWindowOwnerPID as String] as? Int32) == guardedPID && ($0[kCGWindowNumber as String] as? Int) == guardedWindow }) else { print("refuse bound window missing"); exit(3) }
  if event.type == .keyDown || event.type == .keyUp {
    guard let index = list.firstIndex(where: { ($0[kCGWindowNumber as String] as? Int) == guardedWindow }),
          let b = list[index][kCGWindowBounds as String] as? [String: Double] else { print("refuse key window unknown"); exit(3) }
    let rect = CGRect(x:b["X"]!, y:b["Y"]!, width:b["Width"]!, height:b["Height"]!)
    for row in list.prefix(index) {
      let pid = row[kCGWindowOwnerPID as String] as? Int32 ?? -1
      if pid == guardedPID || guardedAllow.contains(pid) { continue }
      guard let other = row[kCGWindowBounds as String] as? [String: Double] else { print("refuse unknown overlay"); exit(3) }
      let otherRect = CGRect(x:other["X"]!,y:other["Y"]!,width:other["Width"]!,height:other["Height"]!)
      if dockLayer(row, otherRect) { continue }
      if rect.intersects(otherRect) { print("refuse key foreign overlay"); exit(3) }
    }
    return
  }
  let point = event.location
  guard let screen = NSScreen.screens.first(where: { cgFrame($0.frame).contains(point) }) else { print("refuse screen unknown"); exit(3) }
  guard cgFrame(screen.visibleFrame).contains(point) else { print("refuse outside visible frame"); exit(3) }
  for row in list {
    guard let b = row[kCGWindowBounds as String] as? [String: Double] else { continue }
    let rect = CGRect(x:b["X"]!, y:b["Y"]!, width:b["Width"]!, height:b["Height"]!)
    if !rect.contains(point) { continue }
    let pid = row[kCGWindowOwnerPID as String] as? Int32 ?? -1
    if pid == guardedPID || guardedAllow.contains(pid) { return }
    if dockLayer(row, rect) { continue }
    print("refuse foreign overlay"); exit(3)
  }
  print("refuse no owned window at event point"); exit(3)
}
func post(_ e: CGEvent?) {
  guard let event = e else { print("refuse event creation"); exit(3) }
  checkEvent(event)
  event.post(tap: .cghidEventTap)
}
func mouse(_ type: CGEventType, _ x: Double, _ y: Double, button: CGMouseButton = .left, flags: CGEventFlags = [], clicks: Int64 = 1) {
  let e = CGEvent(mouseEventSource: source, mouseType: type, mouseCursorPosition: CGPoint(x: x, y: y), mouseButton: button)
  e?.setIntegerValueField(.mouseEventClickState, value: clicks)
  e?.flags = flags
  post(e)
}
func parseFlags(_ names: [String]) -> CGEventFlags {
  var f: CGEventFlags = []
  for n in names {
    switch n {
    case "cmd": f.insert(.maskCommand)
    case "shift": f.insert(.maskShift)
    case "alt": f.insert(.maskAlternate)
    case "ctrl": f.insert(.maskControl)
    default: break
    }
  }
  return f
}
func wheel(_ dy: Int32, phase: Int64) {
  let e = CGEvent(scrollWheelEvent2Source: source, units: .pixel, wheelCount: 1, wheel1: dy, wheel2: 0, wheel3: 0)
  e?.setIntegerValueField(.scrollWheelEventIsContinuous, value: 1)
  e?.setIntegerValueField(.scrollWheelEventScrollPhase, value: phase)
  post(e)
}
// schedule: [[durationMs, pixelsPerSecond], ...]. Positive speed scrolls content down
// (wheel delta negative). A zero-speed segment ends the current gesture.
func runSchedule(_ schedule: [[Double]], hz: Double) -> (posted: Int, pixels: Double, lateMax: Double) {
  let tick = 1000.0 / hz
  var posted = 0, total = 0.0, lateMax = 0.0
  let start = DispatchTime.now().uptimeNanoseconds
  var at = 0.0
  var carry = 0.0
  var active = false
  for segment in schedule {
    let (duration, speed) = (segment[0], segment[1])
    let end = at + duration
    while at < end {
      let now = Double(DispatchTime.now().uptimeNanoseconds - start) / 1e6
      if now < at { sleepMs(at - now) }
      let actual = Double(DispatchTime.now().uptimeNanoseconds - start) / 1e6
      lateMax = max(lateMax, actual - at)
      if speed != 0 {
        carry += -speed * tick / 1000
        let delta = carry.rounded(.towardZero)
        carry -= delta
        wheel(Int32(delta), phase: active ? 2 : 1)
        active = true
        total += abs(delta)
        posted += 1
      } else if active {
        wheel(0, phase: 4)
        active = false
      }
      at += tick
    }
  }
  if active { wheel(0, phase: 4) }
  return (posted, total, lateMax)
}
setvbuf(stdout, nil, _IOLBF, 0)
while let line = readLine() {
  guard let data = line.data(using: .utf8), let c = try? JSONSerialization.jsonObject(with: data) as? [String: Any], let op = c["op"] as? String else {
    print("err bad"); continue
  }
  if op != "quit" {
    guardedPID = Int32(c["guardPid"] as? Int ?? 0)
    guardedWindow = c["guardWindow"] as? Int ?? 0
    guardedAllow = Set((c["guardAllow"] as? [Int] ?? []).map { Int32($0) })
  }
  let x = c["x"] as? Double ?? 0, y = c["y"] as? Double ?? 0
  let flags = parseFlags(c["flags"] as? [String] ?? [])
  switch op {
  case "move":
    mouse(.mouseMoved, x, y); print("ok")
  case "click":
    let n = c["count"] as? Int ?? 1
    mouse(.mouseMoved, x, y); sleepMs(30)
    for i in 1...n {
      mouse(.leftMouseDown, x, y, flags: flags, clicks: Int64(i)); sleepMs(25)
      mouse(.leftMouseUp, x, y, flags: flags, clicks: Int64(i)); sleepMs(30)
    }
    print("ok")
  case "rightclick":
    mouse(.mouseMoved, x, y); sleepMs(30)
    let down = CGEvent(mouseEventSource: source, mouseType: .rightMouseDown, mouseCursorPosition: CGPoint(x: x, y: y), mouseButton: .right)
    down?.setIntegerValueField(.mouseEventButtonNumber, value: 1)
    down?.setIntegerValueField(.mouseEventClickState, value: 1)
    post(down); sleepMs(25)
    let up = CGEvent(mouseEventSource: source, mouseType: .rightMouseUp, mouseCursorPosition: CGPoint(x: x, y: y), mouseButton: .right)
    up?.setIntegerValueField(.mouseEventButtonNumber, value: 1)
    up?.setIntegerValueField(.mouseEventClickState, value: 1)
    post(up); print("ok")
  case "drag":
    let x2 = c["x2"] as? Double ?? x, y2 = c["y2"] as? Double ?? y
    let duration = c["duration"] as? Double ?? 800
    mouse(.mouseMoved, x, y); sleepMs(150)
    mouse(.leftMouseDown, x, y, flags: flags); sleepMs(c["hold"] as? Double ?? 150)
    let steps = max(10, Int(duration / 16))
    for i in 1...steps {
      let t = Double(i) / Double(steps)
      mouse(.leftMouseDragged, x + (x2 - x) * t, y + (y2 - y) * t, flags: flags)
      sleepMs(16)
    }
    sleepMs(c["settle"] as? Double ?? 350)
    mouse(.leftMouseDragged, x2, y2, flags: flags); sleepMs(60)
    mouse(.leftMouseUp, x2, y2, flags: flags)
    print("ok")
  case "text":
    // Layout-independent characters: the key event carries the string itself.
    for ch in (c["text"] as? String ?? "") {
      let units = Array(String(ch).utf16)
      let d = CGEvent(keyboardEventSource: source, virtualKey: 0, keyDown: true)
      d?.keyboardSetUnicodeString(stringLength: units.count, unicodeString: units)
      post(d); sleepMs(20)
      let u = CGEvent(keyboardEventSource: source, virtualKey: 0, keyDown: false)
      u?.keyboardSetUnicodeString(stringLength: units.count, unicodeString: units)
      post(u); sleepMs(40)
    }
    print("ok")
  case "key":
    let code = CGKeyCode(c["code"] as? Int ?? 0)
    let d = CGEvent(keyboardEventSource: source, virtualKey: code, keyDown: true); d?.flags = flags; post(d)
    sleepMs(20)
    let u = CGEvent(keyboardEventSource: source, virtualKey: code, keyDown: false); u?.flags = flags; post(u)
    print("ok")
  case "ctrlwheel":
    let count = c["count"] as? Int ?? 1
    let dy = Int32(c["dy"] as? Int ?? -40)
    for _ in 0..<count {
      let e = CGEvent(scrollWheelEvent2Source: source, units: .pixel, wheelCount: 1, wheel1: dy, wheel2: 0, wheel3: 0)
      e?.flags = .maskControl
      post(e)
      sleepMs(c["interval"] as? Double ?? 16)
    }
    print("ok")
  case "scroll":
    let r = runSchedule(c["schedule"] as? [[Double]] ?? [], hz: c["hz"] as? Double ?? 120)
    print("ok posted=\(r.posted) pixels=\(Int(r.pixels)) lateMaxMs=\(String(format: "%.2f", r.lateMax))")
  case "quit":
    print("ok"); exit(0)
  default:
    print("err op")
  }
  fflush(stdout)
}
