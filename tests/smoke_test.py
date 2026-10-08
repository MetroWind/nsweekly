"""Exercise production wiring with a local OpenID stub and a legacy database.

Run: python3 tests/smoke_test.py build/nsweekly
All writable state is isolated in a temporary directory.
"""

import datetime
import http.client
import http.server
import json
from pathlib import Path
import socket
import sqlite3
import subprocess
import sys
import tempfile
import threading
import time
from urllib.parse import urlencode


class OpenIDProvider(http.server.BaseHTTPRequestHandler):
    """Supply local metadata, tokens, and the test user's identity."""

    def do_GET(self):
        """Handle metadata discovery and identity lookup."""
        prefix = f"http://127.0.0.1:{self.server.server_port}"
        if self.path == "/.well-known/openid-configuration":
            data = {
                "authorization_endpoint": prefix + "/login",
                "token_endpoint": prefix + "/token",
                "introspection_endpoint": prefix + "/introspect",
                "userinfo_endpoint": prefix + "/userinfo",
            }
        else:
            data = {"sub": "test-id", "preferred_username": "mw"}
        self.respond(data)

    def do_POST(self):
        """Exchange either a test code or a refresh token."""
        self.rfile.read(int(self.headers.get("Content-Length", "0")))
        self.respond({"access_token": "access", "refresh_token": "refresh"})

    def respond(self, data):
        """Send a complete JSON response."""
        body = json.dumps(data).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *_):
        """Keep smoke output limited to its outcome."""


def request(port, path, method="GET", body=None, cookie=None):
    """Make one request without following redirects."""
    headers = {}
    if cookie:
        headers["Cookie"] = cookie
    if method == "POST" and path.startswith("/games/"):
        headers["Origin"] = f"http://127.0.0.1:{port}"
    if body is not None:
        headers["Content-Type"] = "application/x-www-form-urlencoded"
    connection = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
    try:
        connection.request(method, path, body, headers)
        response = connection.getresponse()
        return response.status, response.getheaders(), response.read().decode()
    finally:
        connection.close()


def stop(process):
    """Stop only the service process launched by this test."""
    process.terminate()
    try:
        process.wait(timeout=5)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=5)


def start(binary, config, port, log):
    """Launch the binary and wait for the local root route to respond."""
    process = subprocess.Popen([binary, "-c", str(config)],
                               stdout=log, stderr=log)
    for _ in range(100):
        if process.poll() is not None:
            raise RuntimeError(f"Startup exited with {process.returncode}")
        try:
            request(port, "/")
            return process
        except OSError:
            time.sleep(0.05)
    stop(process)
    raise RuntimeError("Startup did not become ready")


def main():
    """Verify production wiring, persistence, and startup failures."""
    binary = str(Path(sys.argv[1]).resolve())
    root = Path(__file__).resolve().parent.parent
    provider = http.server.ThreadingHTTPServer(("127.0.0.1", 0),
                                               OpenIDProvider)
    provider_thread = threading.Thread(target=provider.serve_forever)
    provider_thread.start()
    try:
        with tempfile.TemporaryDirectory(prefix="nsweekly_smoke_") as directory:
            data_dir = Path(directory)
            for name in ["templates", "statics"]:
                (data_dir / name).symlink_to(root / name)
            with socket.socket() as listener:
                listener.bind(("127.0.0.1", 0))
                port = listener.getsockname()[1]
            config = data_dir / "nsweekly.yaml"
            values = {
                "data-dir": str(data_dir), "listen-address": "127.0.0.1",
                "listen-port": port, "client-id": "smoke-client",
                "client-secret": "test-secret",
                "openid-url-prefix":
                    f"http://127.0.0.1:{provider.server_port}",
                "url-prefix": f"http://127.0.0.1:{port}",
                "guest-index": "user-weekly", "guest-index-user": "mw",
                "default-lang": "en-US",
            }
            config.write_text("".join(
                f"{key}: {json.dumps(value)}\n"
                for key, value in values.items()))
            db_path = data_dir / "data.db"
            now = datetime.datetime.now(datetime.timezone.utc)
            monday = (now - datetime.timedelta(days=now.weekday())).replace(
                hour=0, minute=0, second=0, microsecond=0)
            date = monday.date().isoformat()
            with sqlite3.connect(db_path) as db:
                db.executescript("""
                    CREATE TABLE Users
                    (id INTEGER PRIMARY KEY ASC, name TEXT UNIQUE);
                    CREATE TABLE Weeklies
                    (user_id INTEGER REFERENCES Users (id) ON DELETE CASCADE,
                     week_start INTEGER, update_time INTEGER, format INTEGER,
                     lang TEXT, content TEXT, UNIQUE (user_id, week_start));
                    INSERT INTO Users VALUES (42, 'mw');
                """)
                db.execute("INSERT INTO Weeklies VALUES (?, ?, ?, ?, ?, ?)",
                           (42, int(monday.timestamp()), int(now.timestamp()),
                            0, "en-US", "**legacy**"))
            with (data_dir / "service.log").open("w+") as log:
                process = start(binary, config, port, log)
                try:
                    status, headers, _ = request(port, "/")
                    assert status == 301
                    assert ("Location", "/weekly/mw") in headers
                    status, headers, _ = request(port, "/login")
                    assert status == 301
                    assert "client_id=smoke-client" in dict(headers)["Location"]
                    status, headers, _ = request(
                        port, "/openid-redirect?code=test-code")
                    assert status == 301
                    assert len([h for h in headers if h[0] == "Set-Cookie"]) == 2
                    assert request(port, "/weekly/mw")[0] == 200
                    single = request(port, f"/weekly/mw/{date}")
                    assert single[0] == 200
                    assert "<strong>legacy</strong>" in single[2]
                    edit = request(port, f"/edit/mw/{date}",
                                   cookie="access-token=access")
                    assert edit[0] == 200
                    assert "**legacy**" in edit[2]
                    assert "/statics/preview.js" in edit[2]
                    assert request(port, "/statics/preview.js")[0] == 200
                    saved = request(port, f"/edit/mw/{date}", "POST",
                                    urlencode({"content": "**saved**"}),
                                    "access-token=access")
                    assert saved[0] == 302
                    assert ("Location", "/") in saved[1]
                    refreshed = request(port, "/", cookie="refresh-token=refresh")
                    assert refreshed[0] == 302
                    cookies = [h for h in refreshed[1]
                               if h[0] == "Set-Cookie"]
                    assert len(cookies) == 2
                    assert request(port, "/games")[0] == 308
                    assert request(port, "/games/")[0] == 302
                    assert request(port, "/games/mw")[0] == 200
                    assert request(port, "/games/unknown")[0] == 404
                    assert request(port, "/games/mw/new")[0] == 401
                    cookie = "access-token=access"
                    assert request(port, "/games/mw/new", cookie=cookie)[0] == 200
                    created = request(port, "/games/mw/new", "POST",
                        urlencode({"name": "Smoke Game", "status": "now_playing",
                                   "platforms": ["pc", "ps_5", "switch"],
                                   "hours": "0", "notes": "%code{macro}"},
                                  doseq=True), cookie)
                    assert created[0] == 303, created
                    public = request(port, "/games/mw")
                    assert "<code>macro</code>" in public[2]
                    assert "PC, Switch, PS 5" in public[2]
                    with sqlite3.connect(db_path) as db:
                        game_id = db.execute(
                            "SELECT id FROM GameTracking").fetchone()[0]
                    edited = request(port, f"/games/mw/{game_id}/edit", "POST",
                        urlencode({"name": "Renamed Game", "status": "done"}),
                        cookie)
                    assert edited[0] == 303, edited
                    page = request(port, f"/games/mw/{game_id}/delete",
                                   cookie=cookie)
                    assert "Renamed Game" in page[2]
                    deleted = request(port, f"/games/mw/{game_id}/delete", "POST",
                                      "", cookie)
                    assert deleted[0] == 303, deleted
                    assert request(port, f"/games/mw/{game_id}/edit",
                                   cookie=cookie)[0] == 404
                    oversized = request(port, "/games/mw/new", "POST",
                                        "notes=" + "a" * (1024 * 1024), cookie)
                    assert oversized[0] == 413, oversized[0]
                finally:
                    stop(process)
                process = start(binary, config, port, log)
                try:
                    reopened = request(port, f"/weekly/mw/{date}")
                    assert reopened[0] == 200
                    assert "<strong>saved</strong>" in reopened[2]
                finally:
                    stop(process)
                with sqlite3.connect(db_path) as db:
                    id_row = db.execute(
                        "SELECT id FROM Users WHERE name='mw'").fetchone()
                    assert id_row == (42,)
                    count = db.execute(
                        "SELECT COUNT(*) FROM Weeklies").fetchone()
                    assert count == (1,)
                failures = [
                    ({"url-prefix": "invalid"}, 4),
                    ({"openid-url-prefix": ""}, 1),
                    ({"data-dir": str(data_dir / "missing")}, 2),
                ]
                for changes, expected_code in failures:
                    failed_config = data_dir / "failed.yaml"
                    failed_values = values | changes
                    failed_config.write_text("".join(
                        f"{key}: {json.dumps(value)}\n"
                        for key, value in failed_values.items()))
                    failed = subprocess.run(
                        [binary, "-c", str(failed_config)],
                        stdout=log, stderr=log, timeout=5)
                    assert failed.returncode == expected_code
                missing = subprocess.run(
                    [binary, "-c", str(data_dir / "missing.yaml")],
                    stdout=log, stderr=log, timeout=5)
                assert missing.returncode == 3
                # Import uses neither templates nor a reachable provider/listener.
                import_values = values | {
                    "openid-url-prefix": "http://127.0.0.1:1"}
                import_config = data_dir / "import.yaml"
                import_config.write_text("".join(
                    f"{key}: {json.dumps(value)}\n"
                    for key, value in import_values.items()))
                for name in ["templates", "statics"]:
                    (data_dir / name).unlink()
                arguments = [binary, "-c", str(import_config),
                    "--import-games-csv", str(root / "tests/fixtures/tracker.csv"),
                    "--import-games-user", "mw"]
                for count in [3, 0]:
                    imported = subprocess.run(arguments, capture_output=True,
                                              text=True, timeout=10)
                    assert imported.returncode == 0, imported.stdout + imported.stderr
                    assert f"inserted={count}" in imported.stdout, imported.stdout
                assert subprocess.run([binary, "--import-games-user", "mw"],
                    capture_output=True).returncode == 2
                assert subprocess.run([binary, "--help"],
                    capture_output=True).returncode == 0
                print("Production smoke passed: login, guest, pages, preview, "
                      "save, refresh, reopen, games CRUD/import, and startup exit codes.")
    finally:
        provider.shutdown()
        provider_thread.join()
        provider.server_close()


if __name__ == "__main__":
    main()
