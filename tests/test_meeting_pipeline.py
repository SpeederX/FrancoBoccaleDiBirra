import http.server
import json
import pathlib
import subprocess
import sys
import tempfile
import threading
import unittest


BINARY = pathlib.Path(sys.argv[1]).resolve()
TOOL = pathlib.Path(sys.argv[2]).resolve()
sys.argv = [sys.argv[0]]


class Model(http.server.BaseHTTPRequestHandler):
    mode = "valid"
    calls = []

    def log_message(self, *args):
        pass

    def do_POST(self):
        request = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        Model.calls.append(request)
        if "response_format" in request:
            part = json.loads(request["messages"][1]["content"])
            text = json.dumps({
                "utterances": [{"id": u["id"], "text": u["text"]} for u in part["utterances"]],
                "introductions": [{"speaker": 1, "name": "Alice", "utterance": 1,
                                   "quote": "My name is Alice"}],
            })
        else:
            text = "Summary: draft review. Decisions: none established. Action items: Bob, draft, Friday."
        if Model.mode == "invalid":
            text = "{}"
        outer = {"choices": [{"finish_reason": "length" if Model.mode == "truncated" else "stop",
                              "message": {"content": text}}]}
        data = json.dumps(outer).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)


class Pipeline(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.temp.name)
        self.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Model)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        Model.mode = "valid"
        Model.calls = []

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join()
        self.temp.cleanup()

    def run_cli(self, *args):
        return subprocess.run([str(BINARY), *map(str, args)], capture_output=True, text=True,
                              encoding="utf-8", timeout=15)

    def transcribe(self, model="fixture", root=None):
        root = root or self.root
        return self.run_cli("--meeting-transcribe", "--directory", root,
                            "--parakeet", TOOL, "--asr", model, "--diar", "fixture")

    def process(self):
        return self.run_cli("--meeting-process", "--directory", self.root,
                            "--llm-port", self.server.server_port, "--llm-model", "exact-fixture-model")

    def test_bad_arguments_and_corrupt_tool_output(self):
        for args in [("--meeting-transcribe",), ("--meeting-record", "--seconds", "zero"),
                     ("--meeting-process", "--llm-port", "123x"), ("--meeting-unknown",),
                     ("--meeting", "--asr")]:
            self.assertNotEqual(self.run_cli(*args).returncode, 0)
        self.assertNotEqual(self.transcribe("bad").returncode, 0)
        self.assertFalse((self.root / "transcript.json").exists())
        failed = self.transcribe("failed")
        self.assertNotEqual(failed.returncode, 0)
        self.assertIn("exit 7", failed.stderr)

    def test_transcription_cleaning_names_notes_and_separate_report(self):
        self.assertEqual(self.transcribe().returncode, 0)
        notes = "User note: review is tentative.\n" * 400
        (self.root / "notes.txt").write_text(notes, encoding="utf-8")
        result = self.process()
        self.assertEqual(result.returncode, 0, result.stderr)
        doc = json.loads((self.root / "transcript.json").read_text())
        self.assertEqual(doc["speakers"][0]["name"], "Alice")
        self.assertNotIn("name", doc["speakers"][1])
        self.assertTrue(all("cleaned" in u for u in doc["utterances"]))
        self.assertTrue(doc["report_current"])
        self.assertEqual((self.root / "notes.txt").read_text(), notes)
        self.assertEqual((self.root / "notes-used.txt").read_text(), notes)
        for name in ["parakeet.scene.jsonl", "transcript.txt", "transcript-clean.txt", "report.md"]:
            self.assertTrue((self.root / name).is_file())
        self.assertTrue(all(r["model"] == "exact-fixture-model" for r in Model.calls))
        note_requests = [r for r in Model.calls if r["messages"][1]["content"].startswith("User notes:")]
        self.assertGreater(len(note_requests), 1)
        self.assertEqual(self.transcribe().returncode, 0)
        retried = json.loads((self.root / "transcript.json").read_text())
        self.assertEqual(retried["speakers"][0]["name"], "Alice")
        self.assertFalse(retried["report_current"])
        self.assertTrue((self.root / "transcript-previous.json").is_file())

    def test_invalid_or_truncated_llm_cannot_publish_final_report(self):
        self.assertEqual(self.transcribe().returncode, 0)
        for mode in ["invalid", "truncated"]:
            Model.mode = mode
            result = self.process()
            self.assertNotEqual(result.returncode, 0)
            self.assertFalse((self.root / "report.md").exists())
            self.assertTrue((self.root / "parakeet.scene.jsonl").exists())

    def test_failed_import_preserves_source_and_existing_audio(self):
        source = self.root / "source.bin"
        source.write_bytes(b"source must remain unchanged")
        audio = self.root / "audio.wav"
        audio.write_bytes(b"prior audio")
        result = self.run_cli("--meeting-transcribe", "--directory", self.root, "--source", source,
                              "--parakeet", TOOL, "--asr", "fixture", "--diar", "fixture")
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(source.read_bytes(), b"source must remain unchanged")
        self.assertEqual(audio.read_bytes(), b"prior audio")

    @unittest.skipUnless(sys.platform == "win32", "Windows PowerShell launch scripts")
    def test_windows_launcher_and_preparation_validation(self):
        scripts = pathlib.Path(__file__).resolve().parents[1] / "scripts"
        assets = self.root / "assets with spaces"
        assets.mkdir()
        for name in ["parakeet-v0.6.1-bin-win-cpu-x64/parakeet-cli.exe",
                     "tdt-0.6b-v3-q8_0.gguf", "parakeet-bundle-small.gguf"]:
            path = assets / name
            path.parent.mkdir(exist_ok=True)
            path.touch()

        def run_script(name, *args):
            return subprocess.run(["powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass",
                                   "-File", str(scripts / name), *map(str, args)],
                                  capture_output=True, text=True, timeout=15)

        common = ["-Executable", BINARY, "-AssetsDirectory", assets, "-PrintCommand"]
        valid = run_script("start-meeting.ps1", *common, "-LlmPort", 18089, "-LlmModel", "exact-model")
        self.assertEqual(valid.returncode, 0, valid.stderr)
        command = json.loads(valid.stdout)
        self.assertEqual(command[1], "--meeting")
        self.assertIn(str(assets / "tdt-0.6b-v3-q8_0.gguf"), command)
        self.assertEqual(command[-1], "exact-model")
        missing_model = run_script("start-meeting.ps1", *common, "-LlmPort", 18089)
        self.assertNotEqual(missing_model.returncode, 0)
        self.assertIn("Provide both", missing_model.stderr)
        (assets / "parakeet-win-cpu.zip").write_bytes(b"wrong archive")
        bad_hash = run_script("prepare-meeting.ps1", "-AssetsDirectory", assets)
        self.assertNotEqual(bad_hash.returncode, 0)
        self.assertIn("Hash mismatch", bad_hash.stderr)


unittest.main()
