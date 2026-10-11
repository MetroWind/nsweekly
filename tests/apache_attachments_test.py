"""Check the shipped Apache policy against isolated static files.

Run: python3 tests/apache_attachments_test.py [path/to/httpd]
Requires Apache 2.4 and its standard shared modules. Returns 77 if unavailable.
No application backend runs, so successful downloads also verify exclusion
from the catch-all proxy. Actual saved browser filenames need browser checks.
"""

import grp
import hashlib
import http.client
import os
from pathlib import Path
import pwd
import shutil
import socket
import subprocess
import sys
import tempfile
import time
from urllib.parse import quote


def main():
    """Launch a temporary vhost and assert download/security/cache behavior."""
    binary = sys.argv[1] if len(sys.argv) > 1 else (
        shutil.which("httpd") or shutil.which("apache2"))
    if not binary:
        print("SKIP: Apache is not installed")
        return 77
    module_dirs = [Path(path) for path in (
        "/usr/lib/httpd/modules", "/usr/lib64/httpd/modules",
        "/usr/lib/apache2/modules")]
    module_dir = next((path for path in module_dirs if path.is_dir()), None)
    if module_dir is None:
        print("SKIP: Apache shared module directory unavailable")
        return 77
    compiled = subprocess.check_output([binary, "-l"], text=True)
    modules = ["mpm_event", "unixd", "authz_core", "alias", "headers",
               "mime", "proxy", "proxy_http"]
    loads = []
    for module in modules:
        if f"mod_{module}.c" in compiled:
            continue
        path = module_dir / f"mod_{module}.so"
        if not path.exists():
            raise RuntimeError(f"Missing required Apache module: {path}")
        loads.append(f'LoadModule {module}_module "{path}"')
    account = pwd.getpwuid(os.getuid())
    if account.pw_uid == 0:
        account = pwd.getpwnam("nobody")
    group = grp.getgrgid(account.pw_gid).gr_name
    source = Path(__file__).resolve().parent.parent
    with tempfile.TemporaryDirectory(prefix="nsweekly_apache_") as temporary:
        root = Path(temporary)
        root.chmod(0o755)
        storage = root / "attachments"
        storage.mkdir()
        (root / "www").mkdir()
        (root / "mime.types").write_text("")
        (storage / ".staging").mkdir()
        (storage / ".staging" / "secret").write_text("must not be public")
        contents = b"public bytes without a running backend"
        digest = hashlib.sha256(contents).hexdigest()[:32]
        shard = storage / digest[:2]
        shard.mkdir()
        (shard / digest).write_bytes(contents)
        with socket.socket() as probe:
            probe.bind(("127.0.0.1", 0))
            port = probe.getsockname()[1]
        policy = (source / "examples/apache_attachments.conf").read_text()
        policy = policy.replace("/var/lib/nsweekly/attachments", str(storage))
        config = root / "httpd.conf"
        config.write_text("\n".join(loads) + f'''
ServerRoot "{root}"
PidFile "{root}/httpd.pid"
ErrorLog "{root}/error.log"
Listen 127.0.0.1:{port}
ServerName localhost
User {account.pw_name}
Group {group}
DocumentRoot "{root}/www"
TypesConfig "{root}/mime.types"
{policy}
ProxyPass "/" "http://127.0.0.1:1/"
''')
        subprocess.run([binary, "-t", "-f", str(config)], check=True)
        process = subprocess.Popen([binary, "-X", "-f", str(config)],
                                   stdout=subprocess.DEVNULL,
                                   stderr=subprocess.PIPE)

        def request(path, method="GET", headers=None):
            connection = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
            try:
                connection.request(method, path, headers=headers or {})
                response = connection.getresponse()
                return response.status, dict(response.getheaders()), response.read()
            finally:
                connection.close()

        try:
            for _ in range(100):
                if process.poll() is not None:
                    raise RuntimeError((root / "error.log").read_text())
                try:
                    request("/")
                    break
                except OSError:
                    time.sleep(0.05)
            else:
                raise RuntimeError("Apache did not start")
            prefix = f"/attachment/{digest}/"
            for name in ["original.pdf", "café 😀 %.pdf", 'space "quote".txt']:
                result = request(prefix + quote(name, safe=""))
                assert result[0] == 200 and result[2] == contents, result
                assert result[1]["Content-Type"] == "application/octet-stream"
                assert result[1]["Content-Disposition"] == "attachment"
                assert result[1]["X-Content-Type-Options"] == "nosniff"
                assert result[1]["Cache-Control"] == "public, max-age=3600"
            for suffix, mime in [("jpg", "image/jpeg"), ("PNG", "image/png"),
                                 ("webp", "image/webp")]:
                result = request(prefix + "cosmetic." + suffix)
                assert result[1]["Content-Type"] == mime, result
                assert result[1]["Content-Disposition"] == "inline"
            for suffix in ["html", "svg", "php"]:
                result = request(prefix + "active." + suffix)
                assert result[1]["Content-Type"] == "application/octet-stream"
                assert result[1]["Content-Disposition"] == "attachment"
            head = request(prefix + "file.txt", "HEAD")
            assert head[0] == 200 and head[2] == b""
            assert int(head[1]["Content-Length"]) == len(contents)
            ranged = request(prefix + "file.txt", headers={"Range": "bytes=1-3"})
            assert ranged[0] == 206 and ranged[2] == contents[1:4]
            conditional = request(prefix + "file.txt", headers={
                "If-None-Match": head[1]["ETag"]})
            assert conditional[0] == 304
            for path in ["/attachment/.staging/secret", "/attachment/" + digest[:2],
                         "/attachment/" + "0" * 32 + "/missing"]:
                assert request(path)[0] >= 400
            assert request("/attachments/mw")[0] >= 500
            print("Apache attachment mapping, headers, ranges and proxy exclusion passed")
        finally:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=5)
    return 0


if __name__ == "__main__":
    sys.exit(main())
