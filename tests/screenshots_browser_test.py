"""Run screenshot controls in headless Firefox with a local fixture server.

Usage: python3 tests/screenshots_browser_test.py
Requires Firefox with Marionette. Only temporary profiles and fixtures are used.
"""
import base64
import http.server
import json
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time


def main():
    binary = shutil.which("firefox-bin") or shutil.which("firefox")
    if not binary:
        print("SKIP: Firefox unavailable")
        return 77
    no_js = "--nojs" in sys.argv
    source = Path(__file__).resolve().parent.parent
    requests = []
    png = base64.b64decode(
        "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+aD1sAAAAASUVORK5CYII=")

    class Handler(http.server.SimpleHTTPRequestHandler):
        def __init__(self, *args, **kwargs):
            super().__init__(*args, directory=str(source), **kwargs)

        def do_GET(self):
            requests.append(self.path)
            if self.path.startswith("/fixture/"):
                self.send_response(200)
                self.send_header("Content-Type", "image/png")
                self.end_headers()
                self.wfile.write(b"unsupported codec" if "unsupported" in self.path else png)
            else:
                super().do_GET()

        def log_message(self, *args):
            pass

    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    with tempfile.TemporaryDirectory(prefix="nsweekly_screenshots_browser_") as root:
        with socket.socket() as probe:
            probe.bind(("127.0.0.1", 0))
            port = probe.getsockname()[1]
        profile = Path(root)
        (profile / "user.js").write_text(
            f'user_pref("marionette.port", {port});\n' +
            ('user_pref("javascript.enabled", false);\n' if no_js else ""))
        with (profile / "firefox.log").open("w") as log:
            process = subprocess.Popen([binary, "--headless", "--no-remote",
                "--marionette", "--profile", str(profile)], stdout=log, stderr=log)
            try:
                for _ in range(100):
                    try:
                        connection = socket.create_connection(("127.0.0.1", port), 1)
                        break
                    except OSError:
                        time.sleep(.1)
                else:
                    raise RuntimeError("Marionette unavailable")
                connection.settimeout(10)

                def receive():
                    prefix = b""
                    while not prefix.endswith(b":"):
                        chunk = connection.recv(1)
                        if not chunk:
                            raise RuntimeError("Marionette closed")
                        prefix += chunk
                    length = int(prefix[:-1])
                    body = b""
                    while len(body) < length:
                        body += connection.recv(length - len(body))
                    return json.loads(body)

                receive()
                sequence = 0

                def command(name, params=None):
                    nonlocal sequence
                    sequence += 1
                    body = json.dumps([0, sequence, name, params or {}]).encode()
                    connection.sendall(str(len(body)).encode() + b":" + body)
                    result = receive()
                    if result[2]:
                        raise RuntimeError(result[2])
                    return result[3]

                def script(text):
                    return command("WebDriver:ExecuteScript", {
                        "script": text, "args": [], "newSandbox": False})["value"]

                def wait_for(expression):
                    for _ in range(100):
                        if script("return " + expression + ";"):
                            return
                        time.sleep(.05)
                    raise AssertionError(expression)

                command("WebDriver:NewSession", {"capabilities": {}})
                url = f"http://127.0.0.1:{server.server_port}/tests/screenshots_browser.html"
                for width in [1280, 390]:
                    command("WebDriver:SetWindowRect", {"width": width, "height": 900})
                    command("WebDriver:Navigate", {"url": url})
                    assert not any("original" in item or "preview" in item for item in requests)
                    assert script("return document.documentElement.scrollWidth <= window.innerWidth;")
                    assert script("return getComputedStyle(document.querySelector('.ScreenshotGallery')).display === 'flex' && getComputedStyle(document.querySelector('.ScreenshotGallery')).flexWrap === 'wrap';")
                    if no_js:
                        assert script("return document.querySelector('input[type=file]').multiple && document.getElementById('ScreenshotUpload').enctype === 'multipart/form-data';")
                        script("document.querySelector('.ScreenshotPreview').click();")
                        wait_for("window.location.pathname === '/fixture/preview.png'")
                        assert not any("original" in item for item in requests)
                        requests.clear()
                        print(f"PASS: no-JavaScript compressed link and native batch form; width={width}")
                        continue
                    script("document.querySelector('.ScreenshotPreview').click();")
                    wait_for("!document.getElementById('ScreenshotImage').hidden")
                    assert script("return document.getElementById('ScreenshotDialog').open && document.getElementById('ScreenshotTitle').textContent === '山 & Moon.png';")
                    script("document.getElementById('ScreenshotClose').click();")
                    assert script("return document.activeElement === document.querySelector('.ScreenshotPreview');")
                    script("document.querySelectorAll('.ScreenshotPreview')[1].click();")
                    wait_for("document.getElementById('ScreenshotStatus').textContent.includes('could not display')")
                    assert script("return document.getElementById('ScreenshotImage').hidden && document.getElementById('ScreenshotTitle').textContent === 'second.png';")
                    assert not any("original" in item for item in requests)
                    # Escape uses the real keyboard and native dialog behavior.
                    element = command("WebDriver:FindElement", {"using": "css selector", "value": "#ScreenshotClose"})
                    element_id = element["value"]["element-6066-11e4-a52e-4f735466cecf"]
                    command("WebDriver:ElementSendKeys", {"id": element_id, "text": "\ue00c"})
                    wait_for("!document.getElementById('ScreenshotDialog').open")
                    assert script("return document.activeElement === document.querySelectorAll('.ScreenshotPreview')[1];")
                    requests.clear()
                    print(f"PASS: preview, error, focus, Escape, no original fetch; width={width}")
                if no_js:
                    command("WebDriver:DeleteSession")
                    connection.close()
                    return 0
                # Exercise sequential requests, duplicate prevention and retry.
                script("""
window.upload_calls = []; window.upload_active = 0; window.upload_peak = 0;
window.fail_second = true;
window.fetch = async function(url, options)
{
    const name = options.body.get('file').name;
    window.upload_calls.push(name);
    window.upload_peak = Math.max(window.upload_peak, ++window.upload_active);
    await new Promise(resolve => setTimeout(resolve, 50));
    --window.upload_active;
    const ok = name !== 'second.png' || !window.fail_second;
    return {ok, json: async () => ({message: 'Failed safely', already_uploaded: false})};
};
const transfer = new DataTransfer();
transfer.items.add(new File(['one'], 'first.png', {type: 'image/png'}));
transfer.items.add(new File(['two'], 'second.png', {type: 'image/png'}));
document.querySelector('input[type=file]').files = transfer.files;
document.getElementById('ScreenshotUpload').requestSubmit();
document.getElementById('ScreenshotUpload').dispatchEvent(new Event('submit', {cancelable:true}));
""")
                wait_for("!document.getElementById('ScreenshotRetry').hidden")
                assert script("return window.upload_calls.length === 2 && window.upload_peak === 1 && document.getElementById('ScreenshotProgress').value === 2;")
                script("window.fail_second = false; document.getElementById('ScreenshotRetry').click();")
                wait_for("window.upload_calls.length === 3 && !document.querySelector('input[type=file]').disabled")
                assert script("return window.upload_calls.join(',') === 'first.png,second.png,second.png';")
                print("PASS: sequential uploads, duplicate submission prevention and retry")
                command("WebDriver:DeleteSession")
                connection.close()
            finally:
                process.terminate()
                process.wait(timeout=10)
                server.shutdown()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
