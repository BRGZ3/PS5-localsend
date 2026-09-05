#!/usr/bin/env python3
"""Black-box tests for the bounded host HTTP server."""

from __future__ import annotations

import http.client
import hashlib
import json
import os
import re
import socket
import subprocess
import sys
import tempfile
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path


SECURITY_HEADERS = {
    "content-security-policy",
    "x-content-type-options",
    "referrer-policy",
    "x-frame-options",
}


def request(
    port: int,
    method: str,
    path: str,
    body: bytes | None = None,
    headers: dict[str, str] | None = None,
):
    connection = http.client.HTTPConnection("127.0.0.1", port, timeout=3)
    connection.request(method, path, body=body, headers=headers or {})
    response = connection.getresponse()
    payload = response.read()
    headers = {key.lower(): value for key, value in response.getheaders()}
    connection.close()
    return response.status, headers, payload


def request_chunked(
    port: int,
    method: str,
    path: str,
    body: bytes,
    headers: dict[str, str] | None = None,
):
    connection = http.client.HTTPConnection("127.0.0.1", port, timeout=3)
    connection.request(
        method, path, body=body, headers=headers or {}, encode_chunked=True
    )
    response = connection.getresponse()
    payload = response.read()
    response_headers = {key.lower(): value for key, value in response.getheaders()}
    connection.close()
    return response.status, response_headers, payload


def wait_for_port(process: subprocess.Popen[str]) -> int:
    deadline = time.monotonic() + 8
    while time.monotonic() < deadline:
        line = process.stdout.readline()
        if line:
            match = re.fullmatch(r"listening: http://127\.0\.0\.1:(\d+)/\n", line)
            if match:
                return int(match.group(1))
        if process.poll() is not None:
            raise AssertionError(f"server exited early with {process.returncode}")
    raise AssertionError("server did not publish its bound port")


def read_pin_notification(process: subprocess.Popen[str]) -> str:
    assert process.stderr is not None
    deadline = time.monotonic() + 3
    while time.monotonic() < deadline:
        line = process.stderr.readline()
        match = re.search(r"PS5 LocalSend PIN (\d{6}) for 127\.0\.0\.1", line)
        if match:
            return match.group(1)
    raise AssertionError("PIN notification was not emitted")


def test_no_auth_mode(binary: str) -> None:
    upload_temp = tempfile.TemporaryDirectory(
        prefix="ps5localsend-none-upload-", dir=str(Path("/tmp").resolve())
    )
    config_temp = tempfile.TemporaryDirectory(
        prefix="ps5localsend-none-config-", dir=str(Path("/tmp").resolve())
    )
    config_path = Path(config_temp.name) / "config.ini"
    config_text = (
        "port=53317\n"
        "auth_mode=none\n"
        "language=ru\n"
        "pin_ttl_seconds=120\n"
        "session_ttl_seconds=900\n"
        "destination=/mnt/ext0/game-images\n"
        "storage_path=inbox|Incoming|/data/media/incoming\n"
        "storage_path=m2|M.2 SSD|/mnt/ext0/game-images\n"
        "storage_path=usb_games|USB Games|/mnt/usb0/transfers\n"
        "max_file_bytes=21474836480\n"
        "max_files_per_session=100\n"
    )
    # Reproduce editors that prepend a UTF-8 BOM; this must not prevent startup.
    config_path.write_bytes(b"\xef\xbb\xbf" + config_text.encode("utf-8"))
    environment = os.environ.copy()
    environment["PS5LOCALSEND_UPLOAD_DIR"] = upload_temp.name
    environment["PS5LOCALSEND_DATA_DIR"] = config_temp.name
    process = subprocess.Popen(
        [binary, "--port", "0"], stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        text=True, bufsize=1, env=environment
    )
    assert process.stdout is not None
    try:
        port = wait_for_port(process)
        origin = f"http://127.0.0.1:{port}"
        status, _, body = request(port, "GET", "/api/v1/status")
        payload = json.loads(body)
        assert status == 200 and payload["authMode"] == "none"
        assert payload["language"] == "ru"
        assert payload["storage"]["target"] == "m2"
        assert payload["storage"]["path"] == "/mnt/ext0/game-images"
        assert [entry["id"] for entry in payload["storage"]["targets"]] == [
            "inbox", "m2", "usb_games"
        ]
        fast_port = int(payload["capabilities"]["fastUploadPort"])

        status, _, body = request(
            port, "POST", "/api/v1/auth/challenge", headers={"Origin": origin}
        )
        assert status == 409
        assert json.loads(body)["error"]["code"] == "auth_disabled"

        data = b"no-pin-upload"
        metadata = json.dumps({
            "name": "without-pin.bin", "size": len(data),
            "type": "application/octet-stream"
        }, separators=(",", ":")).encode()
        status, _, body = request(
            port, "POST", "/api/v1/uploads", metadata,
            {"Origin": origin, "Content-Type": "application/json"}
        )
        assert status == 201, body
        upload_id = json.loads(body)["uploadId"]
        status, _, body = request(
            fast_port, "PUT", "/api/v1/uploads/" + upload_id, data,
            {"Origin": origin, "Content-Type": "application/octet-stream",
             "Content-Length": str(len(data))}
        )
        assert status == 201, body
        assert (Path(upload_temp.name) / "without-pin.bin").read_bytes() == data

        status, _, body = request(
            port, "POST", "/api/v1/storage", b'{"target":"usb_games"}',
            {"Origin": origin, "Content-Type": "application/json"}
        )
        assert status == 200, body
        assert json.loads(body) == {
            "target": "usb_games", "path": "/mnt/usb0/transfers"
        }
        assert "destination=/mnt/usb0/transfers" in config_path.read_text()
    finally:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=5)
        upload_temp.cleanup()
        config_temp.cleanup()


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: test_http_server.py SERVER_BINARY")
    binary = str(Path(sys.argv[1]).resolve())
    # The host upload manager deliberately rejects symlinked directory
    # components.  macOS exposes the default temporary directory through
    # /var/folders and /tmp through symlinks, so resolve /tmp first to keep
    # the integration fixture portable across macOS and Linux.
    upload_temp = tempfile.TemporaryDirectory(
        prefix="ps5localsend-http-", dir=str(Path("/tmp").resolve())
    )
    config_temp = tempfile.TemporaryDirectory(
        prefix="ps5localsend-config-", dir=str(Path("/tmp").resolve())
    )
    environment = os.environ.copy()
    environment["PS5LOCALSEND_UPLOAD_DIR"] = upload_temp.name
    environment["PS5LOCALSEND_DATA_DIR"] = config_temp.name
    environment["PS5LOCALSEND_UPLOAD_LEASE_SECONDS"] = "1"
    process = subprocess.Popen([binary, "--port", "0"], stdout=subprocess.PIPE,
        stderr=subprocess.PIPE, text=True, bufsize=1, env=environment)
    assert process.stdout is not None
    try:
        port = wait_for_port(process)
        assert 0 < port < 65536
        generated_config = Path(config_temp.name) / "config.ini"
        assert generated_config.is_file()
        assert "destination=/data/ps5localsend/inbox" in generated_config.read_text()
        assert "language=en" in generated_config.read_text()
        assert "storage_path=internal|" in generated_config.read_text()
        assert "storage_path=usb|" in generated_config.read_text()

        status, headers, body = request(port, "GET", "/")
        assert status == 200
        assert headers["content-type"] == "text/html; charset=utf-8"
        assert SECURITY_HEADERS <= headers.keys()
        assert b"PS5 LocalSend" in body
        assert b'<html lang="en">' in body
        assert b"Trusted local network only." in body
        assert "Файлы идут".encode() not in body
        assert b'id="page-title"' not in body
        assert body.count(b'id="pin-code"') == 1
        assert b'maxlength="6"' in body
        assert b'id="receiver-transfer"' in body
        assert b'src="/assets/ps5-console.png"' in body
        assert b'id="storage-select"' in body
        assert b"v1.0" in body

        status, headers, body = request(port, "GET", "/assets/app.css")
        assert status == 200 and len(body) > 100
        assert headers["content-type"] == "text/css; charset=utf-8"
        assert headers["cache-control"] == "no-store"

        status, headers, app_body = request(port, "GET", "/assets/app.js")
        assert status == 200
        assert headers["cache-control"] == "no-store"
        assert b"STATUS_POLL_INTERVAL_MS" in app_body
        assert b"TRANSLATIONS" in app_body and b'language === "ru"' in app_body
        assert "Только доверенная локальная сеть.".encode() in app_body
        assert "Удалить".encode() not in app_body
        assert b"cancelRequested" in app_body
        assert b'"pointerdown"' in app_body and b'"touchstart"' in app_body
        assert b"sessionStorage" in app_body
        assert b"/api/v1/storage" in app_body
        assert b"timing" in app_body and b"transferMs" in app_body and b"clientTransferMs" in app_body
        assert b"PS5:" in app_body and "PS5 приняла".encode() in app_body
        assert b"formatDuration" in app_body
        assert b"lastUpload" in app_body and "Последняя передача".encode() in app_body
        assert b"startUploadTicker" in app_body and b"liveUploadMessage" in app_body
        assert b"instantSpeed" in app_body and b"LIVE_STALL_NOTICE_MS" in app_body
        assert b"LIVE_RATE_WINDOW_MS" in app_body and b"updateInstantSpeed" in app_body
        assert "пауза браузера".encode() in app_body and b"serverReceived" in app_body

        status, headers, image_body = request(port, "GET", "/assets/ps5-console.png")
        assert status == 200
        assert headers["content-type"] == "image/png"
        assert headers["cache-control"] == "public, max-age=3600"
        assert image_body.startswith(b"\x89PNG\r\n\x1a\n")

        status, headers, body = request(port, "GET", "/api/v1/status")
        payload = json.loads(body)
        assert status == 200
        assert headers["cache-control"] == "no-store"
        assert payload["ready"] is True
        assert payload["version"] == "1.0"
        assert payload["capabilities"]["fastUploadMode"] == "direct"
        fast_port = int(payload["capabilities"]["fastUploadPort"])
        assert 0 < fast_port < 65536 and fast_port != port
        assert payload["authMode"] == "pin"
        assert payload["language"] == "en"
        assert payload["limits"]["maxFileBytes"] == 21474836480
        assert payload["lastUpload"] is None
        assert payload["transfer"] == {
            "state": "idle",
            "active": False,
            "name": "",
            "receivedBytes": 0,
            "expectedBytes": 0,
        }
        assert payload["storage"]["target"] == "internal"
        assert payload["storage"]["path"] == "/data/ps5localsend/inbox"
        assert [entry["id"] for entry in payload["storage"]["targets"]] == [
            "internal", "usb"
        ]

        status, headers, body = request(port, "POST", "/api/v1/status")
        assert status == 405
        assert headers["allow"] == "GET"
        assert json.loads(body)["error"]["code"] == "method_not_allowed"

        status, _, body = request(port, "GET", "/not-present")
        assert status == 404
        assert json.loads(body)["error"]["code"] == "not_found"

        status, _, body = request(port, "GET", "/api/v1/status", b"x")
        assert status == 413
        assert json.loads(body)["error"]["code"] == "body_too_large"

        status, _, _ = request(port, "GET", "/" + ("a" * 3000))
        assert status in (400, 414)

        origin = f"http://127.0.0.1:{port}"
        status, _, body = request(
            fast_port,
            "OPTIONS",
            "/api/v1/uploads/00000000000000000000000000000000",
            headers={"Origin": "http://attacker.invalid"},
        )
        assert status == 403
        assert json.loads(body)["error"]["code"] == "origin_forbidden"
        status, _, body = request(port, "POST", "/api/v1/auth/challenge")
        assert status == 403
        assert json.loads(body)["error"]["code"] == "origin_forbidden"

        status, _, body = request(
            port,
            "POST",
            "/api/v1/auth/challenge",
            headers={"Origin": "http://attacker.invalid"},
        )
        assert status == 403

        status, headers, body = request(
            port,
            "POST",
            "/api/v1/auth/challenge",
            headers={"Origin": origin, "Accept": "application/json"},
        )
        challenge = json.loads(body)
        assert status == 201
        assert headers["cache-control"] == "no-store"
        assert re.fullmatch(r"[0-9a-f]{32}", challenge["challengeId"])
        assert challenge["expiresIn"] == 120
        assert "pin" not in challenge and "token" not in challenge
        first_pin = read_pin_notification(process)

        oversized = b"x" * 513
        status, _, body = request(
            port,
            "POST",
            "/api/v1/auth/verify",
            oversized,
            {"Origin": origin, "Content-Type": "application/json"},
        )
        assert status == 413
        assert json.loads(body)["error"]["code"] == "body_too_large"

        invalid_shape = json.dumps(
            {"challengeId": challenge["challengeId"], "pin": first_pin, "extra": True}
        ).encode()
        status, _, body = request(
            port,
            "POST",
            "/api/v1/auth/verify",
            invalid_shape,
            {"Origin": origin, "Content-Type": "application/json"},
        )
        assert status == 400
        assert json.loads(body)["error"]["code"] == "invalid_json"

        verify_body = json.dumps(
            {"challengeId": challenge["challengeId"], "pin": first_pin},
            separators=(",", ":"),
        ).encode()

        status, _, body = request(
            port,
            "POST",
            "/api/v1/auth/verify",
            verify_body,
            {"Origin": "http://attacker.invalid", "Content-Type": "application/json"},
        )
        assert status == 403

        status, headers, body = request(
            port,
            "POST",
            "/api/v1/auth/verify",
            verify_body,
            {"Origin": origin, "Content-Type": "application/json"},
        )
        session = json.loads(body)
        assert status == 200
        assert headers["cache-control"] == "no-store"
        assert session["tokenType"] == "Bearer"
        assert session["expiresIn"] == 900
        assert re.fullmatch(r"[0-9a-f]{64}", session["token"])

        upload_headers = {
            "Origin": origin,
            "Content-Type": "application/json",
            "Authorization": "Bearer " + session["token"],
        }
        for target, expected_path in (
            ("usb", "/mnt/usb0/ps5localsend/inbox"),
            ("internal", "/data/ps5localsend/inbox"),
        ):
            status, _, body = request(
                port,
                "POST",
                "/api/v1/storage",
                json.dumps({"target": target}, separators=(",", ":")).encode(),
                upload_headers,
            )
            storage = json.loads(body)
            assert status == 200
            assert storage == {"target": target, "path": expected_path}
            assert ("destination=" + expected_path) in generated_config.read_text()
            status, _, body = request(port, "GET", "/api/v1/status")
            assert status == 200
            current_storage = json.loads(body)["storage"]
            assert current_storage["target"] == target
            assert current_storage["path"] == expected_path

        status, _, body = request(
            port,
            "POST",
            "/api/v1/storage",
            b'{"target":"unknown"}',
            upload_headers,
        )
        assert status == 400 and json.loads(body)["error"]["code"] == "invalid_storage"
        status, _, body = request(port, "POST", "/api/v1/uploads",
            json.dumps({"name": "noauth", "size": 1, "type": "x"}).encode(),
            {"Origin": origin, "Content-Type": "application/json"})
        assert status == 401 and json.loads(body)["error"]["code"] == "unauthorized"

        for malformed_metadata in (
            b'{"name":"x","size":1,"type":"x",}',
            b'{"name":"x","size":01,"type":"x"}',
        ):
            status, _, body = request(port, "POST", "/api/v1/uploads",
                                      malformed_metadata, upload_headers)
            assert status == 400
            assert json.loads(body)["error"]["code"] == "invalid_upload"

        for bad_name in ("../x", "/x", "..\\x", "%2e%2e%2fx", "bad\x01name"):
            status, _, body = request(port, "POST", "/api/v1/uploads",
                json.dumps({"name": bad_name, "size": 1, "type": "x"}).encode(), upload_headers)
            assert status == 400 and json.loads(body)["error"]["code"] == "invalid_upload"

        def prepare_upload(name: str, data: bytes, digest: str | None = None):
            metadata = {"name": name, "size": len(data), "type": "application/octet-stream"}
            if digest is not None:
                metadata["sha256"] = digest
            status, _, body = request(port, "POST", "/api/v1/uploads",
                json.dumps(metadata, ensure_ascii=False, separators=(",", ":")).encode(), upload_headers)
            assert status == 201, body
            return json.loads(body)

        def put_upload(upload_id: str, data: bytes):
            return request(fast_port, "PUT", "/api/v1/uploads/" + upload_id, data, {
                "Origin": origin, "Content-Type": "application/octet-stream",
                "Authorization": "Bearer " + session["token"],
                "Content-Length": str(len(data)),
            })

        prepared = prepare_upload("status-quote\".bin", b"progress")
        status, _, body = request(port, "GET", "/api/v1/status")
        assert status == 200
        transfer = json.loads(body)["transfer"]
        assert transfer["state"] == "preparing"
        assert transfer["active"] is True
        assert transfer["name"] == "status-quote\".bin"
        assert transfer["receivedBytes"] == 0
        assert transfer["expectedBytes"] == len(b"progress")
        status, _, cancel_body = request(
            port,
            "DELETE",
            "/api/v1/uploads/" + prepared["uploadId"],
            headers=upload_headers,
        )
        assert status == 200, cancel_body

        for name, data in (("empty.bin", b""), ("one.bin", b"x"),
                           ("файл.bin", "данные".encode()),
                           ('quote"name.txt', b"quoted")):
            prepared = prepare_upload(name, data)
            assert prepared["name"] == name
            assert int(prepared["fastUploadPort"]) == fast_port
            status, response_headers, body = put_upload(prepared["uploadId"], data)
            completed = json.loads(body)
            assert status == 201 and completed["sha256"] == hashlib.sha256(data).hexdigest(), (
                name, status, body
            )
            assert response_headers["access-control-allow-origin"] == origin
            timing = completed["timing"]
            assert isinstance(timing["preparationMs"], int)
            assert isinstance(timing["transferMs"], int)
            assert isinstance(timing["writeMs"], int)
            assert isinstance(timing["directIO"], bool)
            status, _, status_body = request(port, "GET", "/api/v1/status")
            last_upload = json.loads(status_body)["lastUpload"]
            assert status == 200
            assert last_upload["bytes"] == len(data)
            assert last_upload["timing"] == timing
            assert (Path(upload_temp.name) / prepared["name"]).read_bytes() == data

        raw = socket.create_connection(("127.0.0.1", port), timeout=3)
        raw.sendall(("PUT /api/v1/uploads/00000000000000000000000000000000 HTTP/1.1\r\n"
                     f"Host: 127.0.0.1:{port}\r\nOrigin: {origin}\r\n"
                     f"Authorization: Bearer {session['token']}\r\n"
                     "Content-Type: application/octet-stream\r\n"
                     "Content-Length: 21474836480\r\nConnection: close\r\n\r\n").encode())
        response = raw.recv(4096)
        raw.close()
        assert b" 404 " in response and b"upload_not_found" in response

        prepared = prepare_upload("one.bin", b"y")
        assert prepared["name"] == "one (1).bin"
        status, _, _ = put_upload(prepared["uploadId"], b"y")
        assert status == 201 and (Path(upload_temp.name) / "one.bin").read_bytes() == b"x"

        prepared = prepare_upload("wrong-length.bin", b"abc")
        status, _, body = request(port, "PUT", "/api/v1/uploads/" + prepared["uploadId"], b"ab", {
            "Origin": origin, "Content-Type": "application/octet-stream",
            "Authorization": "Bearer " + session["token"], "Content-Length": "2"})
        assert status == 400 and json.loads(body)["error"]["code"] == "size_mismatch"
        assert not list(Path(upload_temp.name).glob(
            ".ps5localsend-*.part"))

        prepared = prepare_upload("wrong-hash.bin", b"abc", "0" * 64)
        status, _, body = put_upload(prepared["uploadId"], b"abc")
        assert status == 422 and json.loads(body)["error"]["code"] == "hash_mismatch"
        assert not (Path(upload_temp.name) / "wrong-hash.bin").exists()

        status, _, status_body = request(port, "GET", "/api/v1/status")
        capabilities = json.loads(status_body)["capabilities"]
        assert status == 200 and capabilities["chunkUpload"] is True
        chunk_size = int(capabilities["chunkSize"])
        chunk_parallelism = int(capabilities["chunkParallelism"])
        assert int(capabilities["fastUploadPort"]) == fast_port
        assert chunk_size >= 1024 * 1024
        assert chunk_parallelism == 6

        # The parallel transport accepts aligned, out-of-order extents on
        # separate connections and drains them through the ordered writer.
        chunk_data = bytes((index * 17) % 251 for index in range(
            chunk_size * chunk_parallelism + 12345
        ))
        prepared = prepare_upload("parallel.bin", chunk_data)

        status, preflight_headers, body = request(
            fast_port,
            "OPTIONS",
            "/api/v1/uploads/" + prepared["uploadId"],
            headers={
                "Origin": origin,
                "Access-Control-Request-Method": "PUT",
                "Access-Control-Request-Headers":
                    "authorization,content-type,content-range",
            },
        )
        assert status == 204 and body == b""
        assert preflight_headers["access-control-allow-origin"] == origin
        assert "PUT" in preflight_headers["access-control-allow-methods"]

        def put_chunk(index: int):
            start = index * chunk_size
            end = min(len(chunk_data), start + chunk_size)
            return request(
                fast_port,
                "PUT",
                "/api/v1/uploads/" + prepared["uploadId"],
                chunk_data[start:end],
                {
                    "Origin": origin,
                    "Content-Type": "application/octet-stream",
                    "Authorization": "Bearer " + session["token"],
                    "Content-Range": f"bytes {start}-{end - 1}/{len(chunk_data)}",
                    "Content-Length": str(end - start),
                },
            )

        chunk_count = (len(chunk_data) + chunk_size - 1) // chunk_size
        with ThreadPoolExecutor(max_workers=chunk_parallelism) as executor:
            chunk_responses = list(executor.map(put_chunk, range(chunk_count)))
        for status, _, body in chunk_responses:
            payload = json.loads(body)
            assert status == 201 and payload["chunk"] is True
        status, _, body = request(port, "GET", "/api/v1/status")
        assert status == 200
        assert json.loads(body)["transfer"]["receivedBytes"] == len(chunk_data)
        status, _, body = request(
            port,
            "POST",
            "/api/v1/uploads/" + prepared["uploadId"] + "/complete",
            headers=upload_headers,
        )
        completed = json.loads(body)
        assert status == 201
        assert completed["sha256"] == hashlib.sha256(chunk_data).hexdigest()
        assert (Path(upload_temp.name) / prepared["name"]).read_bytes() == chunk_data

        # A committed extent can still be discarded with the same cancellation
        # endpoint; no temporary part may survive the cancellation.
        cancel_data = b"z" * (chunk_size + 1)
        prepared = prepare_upload("parallel-cancel.bin", cancel_data)
        status, _, _ = request_chunked(
            port,
            "PUT",
            "/api/v1/uploads/" + prepared["uploadId"],
            cancel_data[:chunk_size],
            {
                "Origin": origin,
                "Content-Type": "application/octet-stream",
                "Authorization": "Bearer " + session["token"],
                "Content-Range": f"bytes 0-{chunk_size - 1}/{len(cancel_data)}",
            },
        )
        assert status == 201
        status, _, _ = request(
            port, "DELETE", "/api/v1/uploads/" + prepared["uploadId"],
            headers=upload_headers,
        )
        assert status == 200
        assert not list(Path(upload_temp.name).glob(".ps5localsend-*.part"))

        prepared = prepare_upload("no-length.bin", b"")
        raw = socket.create_connection(("127.0.0.1", port), timeout=3)
        raw.sendall((f"PUT /api/v1/uploads/{prepared['uploadId']} HTTP/1.1\r\n"
                     f"Host: 127.0.0.1:{port}\r\nOrigin: {origin}\r\n"
                     f"Authorization: Bearer {session['token']}\r\n"
                     "Content-Type: application/octet-stream\r\nConnection: close\r\n\r\n").encode())
        chunks = []
        while True:
            chunk = raw.recv(4096)
            if not chunk:
                break
            chunks.append(chunk)
        response = b"".join(chunks)
        raw.close()
        assert b" 411 " in response and b"length_required" in response
        assert not list(Path(upload_temp.name).glob(
            ".ps5localsend-*.part"))

        prepared = prepare_upload("abandoned.bin", b"x")
        status, _, body = request(port, "POST", "/api/v1/uploads",
            json.dumps({"name": "blocked.bin", "size": 1,
                        "type": "application/octet-stream"}).encode(), upload_headers)
        assert status == 409 and json.loads(body)["error"]["code"] == "upload_busy"
        time.sleep(1.05)
        replacement = prepare_upload("after-lease.bin", b"x")
        status, _, _ = request(port, "DELETE", "/api/v1/uploads/" + replacement["uploadId"], headers=upload_headers)
        assert status == 200
        assert not list(Path(upload_temp.name).glob(
            ".ps5localsend-*.part"))

        prepared = prepare_upload("disconnect.bin", b"0123456789")
        raw = socket.create_connection(("127.0.0.1", fast_port), timeout=3)
        raw.sendall((f"PUT /api/v1/uploads/{prepared['uploadId']} HTTP/1.1\r\n"
                     f"Host: 127.0.0.1:{fast_port}\r\nOrigin: {origin}\r\n"
                     f"Authorization: Bearer {session['token']}\r\n"
                     "Content-Type: application/octet-stream\r\nContent-Length: 10\r\n\r\n").encode() + b"012")
        raw.close()
        deadline = time.monotonic() + 3
        while time.monotonic() < deadline and list(
            Path(upload_temp.name).glob(".ps5localsend-*.part")):
            time.sleep(0.02)
        assert not list(Path(upload_temp.name).glob(
            ".ps5localsend-*.part"))
        assert not (Path(upload_temp.name) / "disconnect.bin").exists()

        oversized_metadata = json.dumps({"name": "huge.bin", "size": 21474836481,
                                          "type": "application/octet-stream"}).encode()
        status, _, body = request(port, "POST", "/api/v1/uploads", oversized_metadata, upload_headers)
        assert status == 413 and json.loads(body)["error"]["code"] == "file_too_large"

        status, _, body = request(
            port,
            "POST",
            "/api/v1/auth/verify",
            verify_body,
            {"Origin": origin, "Content-Type": "application/json"},
        )
        assert status == 401
        assert json.loads(body)["error"]["code"] == "challenge_used"

        abandoned = prepare_upload("old-auth.bin", b"x")
        status, _, body = request(port, "POST", "/api/v1/auth/challenge",
                                  headers={"Origin": origin})
        assert status == 201
        challenge = json.loads(body)
        replacement_pin = read_pin_notification(process)
        replacement_body = json.dumps({"challengeId": challenge["challengeId"],
                                       "pin": replacement_pin}).encode()
        status, _, body = request(port, "POST", "/api/v1/auth/verify",
            replacement_body, {"Origin": origin, "Content-Type": "application/json"})
        assert status == 200
        session = json.loads(body)
        upload_headers["Authorization"] = "Bearer " + session["token"]
        replacement = prepare_upload("new-auth.bin", b"x")
        status, _, _ = request(port, "DELETE", "/api/v1/uploads/" + replacement["uploadId"], headers=upload_headers)
        assert status == 200
        assert not list(Path(upload_temp.name).glob(
            ".ps5localsend-*.part"))

        status, _, body = request(
            port,
            "POST",
            "/api/v1/auth/challenge",
            headers={"Origin": origin},
        )
        assert status == 201
        challenge = json.loads(body)
        second_pin = read_pin_notification(process)
        wrong_pin = "000000" if second_pin != "000000" else "111111"
        wrong_body = json.dumps(
            {"challengeId": challenge["challengeId"], "pin": wrong_pin}
        ).encode()
        for attempt in range(5):
            status, _, body = request(
                port,
                "POST",
                "/api/v1/auth/verify",
                wrong_body,
                {"Origin": origin, "Content-Type": "application/json"},
            )
            assert status == (429 if attempt == 4 else 401)
        assert json.loads(body)["error"]["code"] == "too_many_attempts"
        status, _, body = request(
            port,
            "POST",
            "/api/v1/auth/challenge",
            headers={"Origin": origin},
        )
        assert status == 429
        assert json.loads(body)["error"]["code"] == "challenge_rate_limited"

        with socket.create_connection(("127.0.0.1", port), timeout=3) as raw:
            raw.sendall(b"NOT HTTP\r\n\r\n")
            raw.shutdown(socket.SHUT_WR)
            malformed = raw.recv(256)
        assert malformed == b"" or malformed.startswith(b"HTTP/1.1 400")
    finally:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=5)
        if process.returncode not in (0, -15):
            stderr = process.stderr.read() if process.stderr is not None else ""
            raise AssertionError(f"server exit {process.returncode}: {stderr}")
        upload_temp.cleanup()
        config_temp.cleanup()
    test_no_auth_mode(binary)
    print("HTTP integration tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
