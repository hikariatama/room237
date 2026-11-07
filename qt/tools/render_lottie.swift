// render_lottie LOTTIE_JS ANIMATION_JSON OUT_DIR SIZE: renders every frame of a Lottie file with the original
// lottie-web canvas renderer, so the Qt app plays exactly the animation the web app played.
import AppKit
import WebKit
let args = CommandLine.arguments
let script = try! String(contentsOfFile: args[1], encoding: .utf8)
let animation = try! String(contentsOfFile: args[2], encoding: .utf8)
let out = args[3]
let size = Int(args[4])!
try? FileManager.default.createDirectory(atPath: out, withIntermediateDirectories: true)
let app = NSApplication.shared
app.setActivationPolicy(.prohibited)
final class Runner: NSObject, WKNavigationDelegate {
  let web = WKWebView(frame: NSRect(x: 0, y: 0, width: size, height: size))
  func webView(_ view: WKWebView, didFinish navigation: WKNavigation!) {
    let setup = """
    (function(){ window.anim = lottie.loadAnimation({container: document.getElementById('c'), renderer: 'canvas', loop: false, autoplay: false,
      animationData: \(animation), rendererSettings: {clearCanvas: true}}); return true; })()
    """
    view.evaluateJavaScript(setup) { _, error in
      if let error { print(error); exit(1) }
      DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) { self.frame(0) }
    }
  }
  func frame(_ index: Int) {
    let js = "(function(){var total=Math.floor(anim.totalFrames); if(\(index)>=total) return 'done'; anim.goToAndStop(\(index), true); return document.querySelector('canvas').toDataURL('image/png');})()"
    web.evaluateJavaScript(js) { result, error in
      guard let text = result as? String, error == nil else { print("error", error ?? "none"); exit(1) }
      if text == "done" { print("frames \(index)"); exit(0) }
      let data = Data(base64Encoded: String(text.dropFirst("data:image/png;base64,".count)))!
      try! data.write(to: URL(fileURLWithPath: "\(out)/\(String(format: "%04d", index)).png"))
      self.frame(index + 1)
    }
  }
}
let runner = Runner()
runner.web.navigationDelegate = runner
runner.web.loadHTMLString("<html><body style='margin:0;background:transparent'><div id='c' style='width:\(size)px;height:\(size)px'></div><script>\(script)</script></body></html>", baseURL: nil)
app.run()
