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
            if self.path.startswith("/fixture/fit-"):
                width, height = ((4000, 1000) if "wide" in self.path
                                 else (320, 180) if "small" in self.path
                                 else (1000, 4000))
                self.send_response(200)
                self.send_header("Content-Type", "image/svg+xml")
                self.end_headers()
                self.wfile.write((f'<svg xmlns="http://www.w3.org/2000/svg" '
                    f'width="{width}" height="{height}">'
                    '<rect width="100%" height="100%" fill="green"/>'
                    '</svg>').encode())
                return
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
                    assert script("return getComputedStyle(document.getElementById('ScreenshotRetry')).display === 'none' && document.querySelector('input[type=file]').closest('.ScreenshotUploadControls') === document.getElementById('ScreenshotRetry').parentElement;")
                    if no_js:
                        assert script("return document.querySelector('input[type=file]').multiple && document.getElementById('ScreenshotUpload').enctype === 'multipart/form-data';")
                        assert script("return document.querySelector('.ScreenshotPreview').hash === '#screenshot-11';")
                        script("document.querySelector('.ScreenshotFallback').click();")
                        wait_for("window.location.pathname === '/fixture/preview.png'")
                        assert not any("original" in item for item in requests)
                        requests.clear()
                        print(f"PASS: no-JavaScript compressed link and native batch form; width={width}")
                        continue
                    script("""
window.saved_clipboard = Object.getOwnPropertyDescriptor(navigator, 'clipboard');
window.saved_copy = document.execCommand;
window.copied_url = '';
Object.defineProperty(navigator, 'clipboard', {configurable: true, value: {
    writeText: async text => { window.copied_url = text; }
}});
document.querySelector('.ScreenshotCopy').click();
""")
                    wait_for("document.getElementById('ScreenshotCopyStatus').textContent.includes('URL copied')")
                    assert script("return window.copied_url === new URL('#screenshot-11', location.href).href && !location.hash && !document.getElementById('ScreenshotDialog').open;")
                    script("""
navigator.clipboard.writeText = async () => { throw new Error('Denied'); };
document.execCommand = function(command)
{
    window.copied_url = document.querySelector('textarea').value;
    return command === 'copy';
};
document.querySelectorAll('.ScreenshotCopy')[1].click();
""")
                    wait_for("document.getElementById('ScreenshotCopyStatus').textContent.includes('second.png')")
                    assert script("return window.copied_url.endsWith('#screenshot-22') && !document.querySelector('textarea') && !document.getElementById('ScreenshotDialog').open;")
                    script("""
if(window.saved_clipboard)
    Object.defineProperty(navigator, 'clipboard', window.saved_clipboard);
else
    delete navigator.clipboard;
document.execCommand = window.saved_copy;
""")
                    coords = script("const rect = document.querySelector('.ScreenshotGallery figure').getBoundingClientRect(); return {x: Math.round(rect.left + rect.width / 2), y: Math.round(rect.top + rect.height / 2)};")
                    command("WebDriver:PerformActions", {"actions": [{
                        "type": "pointer", "id": "copy-hover",
                        "parameters": {"pointerType": "mouse"},
                        "actions": [{"type": "pointerMove", "x": coords['x'],
                                     "y": coords['y'], "duration": 0,
                                     "origin": "viewport"}]}]})
                    wait_for("document.querySelector('.ScreenshotGallery figure').matches(':hover')")
                    assert script("""
const figure = document.querySelector('.ScreenshotGallery figure');
const button = figure.querySelector('.ScreenshotCopy');
const bounds = figure.getBoundingClientRect();
const control = button.getBoundingClientRect();
return getComputedStyle(button).opacity === '1' &&
    getComputedStyle(button).borderRadius === '0px' &&
    Math.abs(control.right - bounds.right) < .1 &&
    Math.abs(control.bottom - bounds.bottom) < .1 &&
    button.querySelector('svg') !== null;
""")
                    command("WebDriver:PerformActions", {"actions": [{
                        "type": "pointer", "id": "copy-hover",
                        "parameters": {"pointerType": "mouse"},
                        "actions": [{"type": "pointerMove", "x": 1,
                                     "y": 1, "duration": 0,
                                     "origin": "viewport"}]}]})
                    assert script("return getComputedStyle(document.querySelector('.ScreenshotCopy')).opacity === '0';")
                    print(f"PASS: copy icon, hover, full URL, clipboard fallback; width={width}")
                    assert script("return document.querySelector('.ScreenshotPreview').hash === '#screenshot-11' && new URL(document.querySelector('.ScreenshotPreview').dataset.preview, location.href).pathname === '/fixture/preview.png';")
                    script("document.querySelector('.ScreenshotPreview').click();")
                    wait_for("!document.getElementById('ScreenshotImage').hidden")
                    assert script("return document.getElementById('ScreenshotDialog').open && document.getElementById('ScreenshotTitle').textContent === '山 & Moon.png';")
                    # Large portrait and landscape previews must fit without
                    # clipping or scrolling, including a short mobile viewport.
                    for viewport in [(width, 900), (900, 390)]:
                        command("WebDriver:SetWindowRect", {
                            "width": viewport[0], "height": viewport[1]})
                        for shape in ["wide", "tall", "small"]:
                            script("document.getElementById('ScreenshotImage').src = "
                                   f"'/fixture/fit-{shape}.svg';")
                            wait_for("document.getElementById('ScreenshotImage').complete && "
                                     "document.getElementById('ScreenshotImage').naturalWidth > 1")
                            assert script("""
const dialog = document.getElementById('ScreenshotDialog');
const image = document.getElementById('ScreenshotImage');
const frame = dialog.getBoundingClientRect();
const bounds = image.getBoundingClientRect();
return dialog.scrollHeight <= dialog.clientHeight &&
    dialog.scrollWidth <= dialog.clientWidth &&
    frame.top >= 0 && frame.bottom <= innerHeight &&
    frame.left >= 0 && frame.right <= innerWidth &&
    bounds.height > 0 && bounds.width > 0 &&
    Math.abs(bounds.top - frame.top) < 1 &&
    Math.abs(bounds.bottom - frame.bottom) < 1 &&
    Math.abs(bounds.left - frame.left) < 1 &&
    Math.abs(bounds.right - frame.right) < 1 &&
    getComputedStyle(image).objectFit === 'scale-down';
""")
                    command("WebDriver:SetWindowRect", {"width": width, "height": 900})
                    wait_for("""(() => {
const small = document.getElementById('ScreenshotImage');
const compact = document.getElementById('ScreenshotDialog');
return Math.abs(small.getBoundingClientRect().width - 320) < 1 &&
    Math.abs(small.getBoundingClientRect().height - 180) < 1 &&
    compact.getBoundingClientRect().width < 390 &&
    compact.getBoundingClientRect().height < 500;
})()""")
                    assert script("""
const dialog = document.getElementById('ScreenshotDialog');
const close = document.getElementById('ScreenshotClose');
const next = document.getElementById('ScreenshotNext');
const frame = dialog.getBoundingClientRect();
const close_bounds = close.getBoundingClientRect();
const next_bounds = next.getBoundingClientRect();
const left = document.getElementById('ScreenshotCloseLeft');
const left_bounds = left.getBoundingClientRect();
const deletion = document.getElementById('ScreenshotDelete');
const delete_bounds = deletion.getBoundingClientRect();
return left_bounds.top === frame.top && left_bounds.left === frame.left &&
    getComputedStyle(left).borderRadius === '0px' &&
    delete_bounds.bottom === frame.bottom && delete_bounds.right === frame.right &&
    deletion.querySelector('svg') !== null && !deletion.hidden &&
    deletion.pathname === '/fixture/delete/first' && getComputedStyle(close).borderRadius === '0px' &&
    close_bounds.top === frame.top && close_bounds.right === frame.right &&
    next_bounds.right === frame.right;
""")
                    assert script("""
const dialog = document.getElementById('ScreenshotDialog');
const close = document.getElementById('ScreenshotClose');
const next = document.getElementById('ScreenshotNext');
const bounds = dialog.getBoundingClientRect();
dialog.focus();
function move(x, y)
{
    dialog.dispatchEvent(new PointerEvent('pointermove', {
        pointerType: 'mouse', clientX: x, clientY: y}));
}
const fine = matchMedia('(hover: hover) and (pointer: fine)').matches;
move(bounds.left + bounds.width / 2, bounds.top + bounds.height / 2);
const center_hidden = !fine || (getComputedStyle(close).opacity === '0' &&
    getComputedStyle(next).opacity === '0');
move(bounds.right - 40, bounds.top + 20);
const close_shown = close.hasAttribute('data-near') &&
    getComputedStyle(close).opacity === '1';
const next_hidden = !fine || getComputedStyle(next).opacity === '0';
move(bounds.right - 40, bounds.top + bounds.height / 2);
const next_shown = next.hasAttribute('data-near') &&
    getComputedStyle(next).opacity === '1';
dialog.dispatchEvent(new PointerEvent('pointerleave'));
return center_hidden && close_shown && next_hidden && next_shown &&
    !next.hasAttribute('data-near');
""")
                    assert script("return document.getElementById('ScreenshotPrevious').disabled && !document.getElementById('ScreenshotNext').disabled && document.getElementById('ScreenshotDelete').pathname === '/fixture/delete/first';")
                    script("document.getElementById('ScreenshotNext').click();")
                    wait_for("document.getElementById('ScreenshotStatus').textContent.includes('could not display')")
                    assert script("return document.getElementById('ScreenshotTitle').textContent === 'second.png' && document.getElementById('ScreenshotOriginal').pathname === '/fixture/second_original.png' && document.getElementById('ScreenshotNext').disabled;")
                    script("document.getElementById('ScreenshotPrevious').click();")
                    wait_for("!document.getElementById('ScreenshotImage').hidden")
                    assert script("return document.getElementById('ScreenshotOriginal').pathname === '/fixture/original.png';")
                    script("document.getElementById('ScreenshotDialog').dispatchEvent(new KeyboardEvent('keydown', {key:'ArrowRight', bubbles:true}));")
                    wait_for("document.getElementById('ScreenshotTitle').textContent === 'second.png'")
                    script("document.getElementById('ScreenshotDialog').dispatchEvent(new KeyboardEvent('keydown', {key:'ArrowLeft', bubbles:true}));")
                    wait_for("!document.getElementById('ScreenshotImage').hidden && document.getElementById('ScreenshotTitle').textContent === '山 & Moon.png'")
                    script("document.getElementById('ScreenshotCloseLeft').click();")
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
                    assert not any("original" in item for item in requests)
                    script("document.querySelector('.ScreenshotPreview').click();")
                    wait_for("!document.getElementById('ScreenshotImage').hidden")
                    command("WebDriver:PerformActions", {"actions": [{
                        "type": "pointer", "id": "mouse",
                        "parameters": {"pointerType": "mouse"},
                        "actions": [
                            {"type": "pointerMove", "x": 1, "y": 1,
                             "duration": 0, "origin": "viewport"},
                            {"type": "pointerDown", "button": 0},
                            {"type": "pointerUp", "button": 0}]}]})
                    wait_for("!document.getElementById('ScreenshotDialog').open")
                    assert script("return document.activeElement === document.querySelector('.ScreenshotPreview');")
                    script("document.querySelector('.ScreenshotPreview').click();")
                    wait_for("!document.getElementById('ScreenshotImage').hidden")
                    assert script("return !document.getElementById('ScreenshotCompressed') && document.getElementById('ScreenshotClose').getAttribute('aria-label') === 'Close preview';")
                    script("document.getElementById('ScreenshotImage').click();")
                    wait_for("window.location.pathname === '/fixture/original.png'")
                    assert any("original.png" in item for item in requests)
                    requests.clear()
                    print(f"PASS: edge-to-edge preview, compact sizing, close, original link; width={width}")
                if no_js:
                    command("WebDriver:DeleteSession")
                    connection.close()
                    return 0
                command("WebDriver:Navigate", {"url": url})
                script("document.querySelector('.ScreenshotPreview').click();")
                wait_for("location.hash === '#screenshot-11' && document.getElementById('ScreenshotDialog').open")
                script("document.getElementById('ScreenshotNext').click();")
                wait_for("location.hash === '#screenshot-22'")
                command("WebDriver:Back")
                wait_for("location.hash === '#screenshot-11' && document.getElementById('ScreenshotTitle').textContent === '山 & Moon.png'")
                command("WebDriver:Back")
                wait_for("!location.hash && !document.getElementById('ScreenshotDialog').open")
                command("WebDriver:Forward")
                wait_for("location.hash === '#screenshot-11' && document.getElementById('ScreenshotDialog').open")
                command("WebDriver:Refresh")
                wait_for("document.getElementById('ScreenshotDialog').open && !document.getElementById('ScreenshotImage').hidden")
                script("document.getElementById('ScreenshotClose').click();")
                wait_for("!location.hash && !document.getElementById('ScreenshotDialog').open")
                command("WebDriver:Navigate", {"url": url + "#screenshot-22"})
                wait_for("document.getElementById('ScreenshotStatus').textContent.includes('could not display')")
                assert script("return document.getElementById('ScreenshotOriginal').pathname === '/fixture/second_original.png';")
                script("location.hash = '#screenshot-missing';")
                wait_for("!document.getElementById('ScreenshotDialog').open")
                print("PASS: stable preview fragments, direct links, reload, back/forward, close, missing IDs")
                command("WebDriver:Navigate", {"url": url})
                # Mixed portrait/landscape rows share heights and fill the
                # container; the final incomplete row is not enlarged.
                script("""
const gallery = document.querySelector('.ScreenshotGallery');
gallery.replaceChildren();
for(const [width, height] of Array(4).fill([[320,180], [180,320], [320,240],
    [320,180], [180,320], [320,240], [320,180], [180,320],
    [320,180], [180,320], [320,240], [320,180], [180,320], [320,240]]).flat())
{
    const figure = document.createElement('figure');
    const link = document.createElement('a');
    link.className = 'ScreenshotPreview';
    const image = document.createElement('img');
    image.width = width;
    image.height = height;
    link.append(image);
    figure.append(link);
    gallery.append(figure);
}
initializeScreenshotGallery();
""")
                row_viewports = [(width, .25) for width in
                    [1280, 390, 900, 901, 902, 903, 2442, 2443]]
                row_viewports.extend([(2442, 0), (2443, 0)])
                for width, adjustment in row_viewports:
                    command("WebDriver:SetWindowRect", {"width": width, "height": 900})
                    # Force fractional container widths, as responsive sizing
                    # and browser zoom can do even at integer viewport widths.
                    script("document.querySelector('.ScreenshotGallery').style.width = "
                           f"'calc(100% - {adjustment}px)';")
                    wait_for("""(() => {
const gallery = document.querySelector('.ScreenshotGallery');
const bounds = gallery.getBoundingClientRect();
const rows = [];
for(const figure of gallery.querySelectorAll('figure'))
{
    const rect = figure.getBoundingClientRect();
    const image = figure.querySelector('img');
    let row = rows.at(-1);
    if(!row || Math.abs(row[0].top - rect.top) > 1)
    {
        row = [];
        rows.push(row);
    }
    row.push(rect);
    if(Math.abs(rect.width / rect.height - image.width / image.height) > .01)
        return false;
}
return document.documentElement.scrollWidth <= innerWidth &&
    rows.length > 1 && rows.every((row, index) =>
        row.every(rect => Math.abs(rect.height - row[0].height) < 1) &&
        row.slice(1).every((rect, i) => Math.abs(rect.left - row[i].right - 1) < 1) &&
        (index === rows.length - 1 ||
         Math.abs(row.at(-1).right - bounds.right) < 1)) &&
    rows.at(-1)[0].height <= 180;
})()""")
                    print(f"PASS: justified rows; viewport={width}, fractional inset={adjustment}")
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
                assert script("return window.upload_calls.length === 2 && window.upload_peak === 1 && document.getElementById('ScreenshotProgress').value === 2 && getComputedStyle(document.getElementById('ScreenshotRetry')).display !== 'none';")
                script("window.fail_second = false; document.getElementById('ScreenshotRetry').click();")
                wait_for("window.upload_calls.length === 3 && !document.querySelector('input[type=file]').disabled")
                assert script("return window.upload_calls.join(',') === 'first.png,second.png,second.png' && getComputedStyle(document.getElementById('ScreenshotRetry')).display === 'none';")
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
