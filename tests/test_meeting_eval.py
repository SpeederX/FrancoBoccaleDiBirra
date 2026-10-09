import copy
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock
import wave


SCRIPT = Path(__file__).resolve().parents[1] / "scripts/meeting_eval.py"
spec = importlib.util.spec_from_file_location("meeting_eval", SCRIPT)
evaluation = importlib.util.module_from_spec(spec)
spec.loader.exec_module(evaluation)
NATIVE = tuple(Path(value).resolve() for value in sys.argv[1:])
sys.argv = [sys.argv[0]]


def wav(path, seconds=1, rate=16000):
    with wave.open(str(path), "wb") as audio:
        audio.setnchannels(1)
        audio.setsampwidth(2)
        audio.setframerate(rate)
        audio.writeframes(b"\0\0" * int(seconds * rate))


class Metrics(unittest.TestCase):
    def test_italian_case_punctuation_accents_and_apostrophes(self):
        self.assertEqual(evaluation.normalized("È l’ERA, di Mario!"), "è l'era di mario")
        self.assertEqual(evaluation.score("È l’era di Mario.", "è l'era di mario")["wer_percent"], 0)
        self.assertEqual(evaluation.score("caffè", "caffe\u0300")["cer_percent"], 0)
        self.assertEqual(evaluation.score("è", "e")["wer_percent"], 100)
        self.assertEqual(evaluation.score("'ciao'", "ciao")["wer_percent"], 0)

    def test_known_substitution_deletion_and_insertion_counts(self):
        cases = [("uno due tre", "uno quattro tre", (1, 0, 0)),
                 ("uno due tre", "uno tre", (0, 1, 0)),
                 ("uno due tre", "uno due quattro tre", (0, 0, 1)),
                 ("uno due tre", "uno quattro cinque tre", (1, 0, 1))]
        for reference, hypothesis, expected in cases:
            with self.subTest(hypothesis=hypothesis):
                words = evaluation.score(reference, hypothesis)["words"]
                self.assertEqual((words["substitutions"], words["deletions"], words["insertions"]), expected)
                self.assertEqual(words["errors"], sum(expected))
                self.assertEqual(words["reference_units"], 3)
        self.assertEqual(evaluation.score("abcd", "abxd")["cer_percent"], 25)

    def test_empty_audio_reference_measures_extra_words_without_dividing_by_zero(self):
        hallucinated = evaluation.score("", "Grazie per la visione")
        self.assertIsNone(hallucinated["wer_percent"])
        self.assertEqual(hallucinated["words"]["insertions"], 4)
        self.assertEqual(evaluation.score("", "")["words"]["errors"], 0)
        self.assertEqual(evaluation.score("ciao", "")["wer_percent"], 100)

    def test_numbers_are_not_rewritten_to_hide_recognition_errors(self):
        self.assertEqual(evaluation.score("15 euro", "cinquanta euro")["wer_percent"], 50)


class Corpus(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.corpus = self.root / "private corpus"
        self.source = self.root / "source.wav"
        wav(self.source)
        self.reference = self.root / "reference.txt"
        self.reference.write_text("È l'era di Mario.\n", encoding="utf-8")
        evaluation.init_corpus(self.corpus, self.root / "fbdb.exe", self.root / "assets")

    def tearDown(self):
        self.temp.cleanup()

    def add(self, case_id="italian", kind="speech"):
        evaluation.add_case(self.corpus, case_id, self.source, self.reference, kind, ["headset"])

    def test_init_add_and_load_preserve_original_files_and_reference_bytes(self):
        before_audio = self.source.read_bytes()
        before_reference = self.reference.read_bytes()
        self.add()
        cases = evaluation.load_cases(self.corpus)
        self.assertEqual(len(cases), 1)
        self.assertEqual(cases[0]["audio_seconds"], 1)
        self.assertEqual(cases[0]["tags"], ["headset"])
        self.assertEqual(self.source.read_bytes(), before_audio)
        self.assertEqual(self.reference.read_bytes(), before_reference)
        self.assertEqual(Path(cases[0]["reference"]["path"]).read_bytes(), before_reference)
        with self.assertRaisesRegex(ValueError, "already exists"):
            self.add()
        with self.assertRaisesRegex(ValueError, "already exists"):
            evaluation.init_corpus(self.corpus, self.root / "fbdb.exe", self.root)

    def test_empty_corpus_missing_reference_and_wrong_audio_format_are_rejected(self):
        with self.assertRaisesRegex(ValueError, "no cases"):
            evaluation.load_cases(self.corpus)
        self.reference.write_text("", encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "checked reference"):
            self.add()
        self.reference.write_text("ciao", encoding="utf-8")
        wav(self.source, rate=8000)
        with self.assertRaisesRegex(ValueError, "16 kHz"):
            self.add()
        self.assertEqual(evaluation.read_json(self.corpus / "cases.json")["cases"], [])

    def test_explicit_silence_case_and_changed_reference_have_distinct_identity(self):
        self.reference.write_text("", encoding="utf-8")
        self.add(kind="silence")
        before = evaluation.load_cases(self.corpus)[0]
        self.assertEqual(before["kind"], "silence")
        Path(before["reference"]["path"]).write_text("unexpected speech", encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "silence"):
            evaluation.load_cases(self.corpus)

    def test_unsafe_ids_duplicate_cases_and_paths_outside_corpus_are_rejected(self):
        with self.assertRaises(ValueError):
            self.add("../escape")
        self.add()
        manifest = evaluation.read_json(self.corpus / "cases.json")
        original = copy.deepcopy(manifest)
        manifest["cases"].append(manifest["cases"][0])
        evaluation.write_json(self.corpus / "cases.json", manifest)
        with self.assertRaisesRegex(ValueError, "duplicate"):
            evaluation.load_cases(self.corpus)
        original["cases"][0]["audio"] = "../source.wav"
        evaluation.write_json(self.corpus / "cases.json", original)
        with self.assertRaisesRegex(ValueError, "inside"):
            evaluation.load_cases(self.corpus)

    def test_invalid_models_and_timeout_do_not_start_a_run(self):
        self.add()
        config = evaluation.read_json(self.corpus / "models.json")
        for timeout in (0, -1, True, "slow", float("inf")):
            config["timeout_seconds"] = timeout
            evaluation.write_json(self.corpus / "models.json", config)
            with self.assertRaisesRegex(ValueError, "timeout_seconds"):
                evaluation.run_comparison(self.corpus, self.corpus / "models.json", self.root / "output")
            self.assertFalse((self.root / "output").exists())

    def test_cli_rejects_missing_arguments_and_invalid_corpus_without_recording(self):
        for args in ([], ["add"], ["run", "--corpus", str(self.corpus)]):
            result = subprocess.run([sys.executable, str(SCRIPT), *args], capture_output=True, timeout=5)
            self.assertEqual(result.returncode, 2)
        (self.corpus / "cases.json").write_text("[]", encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "object"):
            evaluation.load_cases(self.corpus)


class Comparison(unittest.TestCase):
    def item(self, errors=0):
        return {"case_id": "sample", "variant_id": "live-q8", "status": "ok", "kind": "speech",
                "audio": {"sha256": "audio"}, "reference": {"sha256": "reference"},
                "words": {"errors": errors}, "wer_percent": errors * 10, "elapsed_seconds": 2}

    def report(self, *items):
        return {"schema_version": 1, "normalization": evaluation.NORMALIZATION, "results": list(items)}

    def test_baseline_detects_improvement_and_regression(self):
        baseline = self.report(self.item(1))
        worse = evaluation.compare_baseline(self.report(self.item(3)), baseline)[0]
        self.assertEqual(worse["word_error_delta"], 2)
        self.assertEqual(worse["wer_percentage_point_delta"], 20)
        better = evaluation.compare_baseline(self.report(self.item(0)), baseline)[0]
        self.assertEqual(better["word_error_delta"], -1)

    def test_changed_inputs_failures_and_missing_cases_are_not_reported_as_improvements(self):
        baseline = self.report(self.item(3))
        changed = self.item(0)
        changed["reference"]["sha256"] = "edited reference"
        self.assertEqual(evaluation.compare_baseline(self.report(changed), baseline)[0]["status"], "changed_input_or_reference")
        failed = self.item()
        failed["status"] = "error"
        self.assertEqual(evaluation.compare_baseline(self.report(failed), baseline)[0]["status"], "asr_failure")
        self.assertEqual(evaluation.compare_baseline(self.report(), baseline)[0]["status"], "missing_case_or_variant")
        self.assertEqual(evaluation.compare_baseline(self.report(self.item()), self.report())[0]["status"], "new_case_or_variant")
        baseline["normalization"] = "different"
        with self.assertRaisesRegex(ValueError, "normalization"):
            evaluation.compare_baseline(self.report(), baseline)

    def test_invalid_transcripts_are_rejected_and_utterance_labels_are_not_scored(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "scene.jsonl"
            path.write_text(json.dumps({"utterances": [{"speaker": 4, "text": "ciao"}]}) + "\n" +
                            json.dumps({"utterances": [{"speaker": 9, "text": "Mario"}]}), encoding="utf-8")
            self.assertEqual(evaluation.transcript_text(path, True), "ciao\nMario")
            for value in ("not JSON", "{}", '{"utterances":[{}]}', ""):
                path.write_text(value, encoding="utf-8")
                with self.assertRaises(ValueError):
                    evaluation.transcript_text(path, True)

    def test_run_failure_retains_logs_and_cannot_publish_a_score(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            case = {"id": "sample", "kind": "speech", "audio": {"path": "audio.wav"},
                    "reference": {}, "audio_seconds": 1, "reference_text": "ciao"}
            variant = {"id": "review", "mode": "review", "files": {
                name: {"path": name} for name in ("parakeet", "asr", "diar")}}
            with mock.patch.object(evaluation, "execute", return_value=(7, 1.5, 100)):
                result = evaluation.run_case(case, variant, root / "result", 10)
            self.assertEqual(result["status"], "error")
            self.assertNotIn("wer_percent", result)
            self.assertEqual(result["exit_code"], 7)
            self.assertTrue((root / "result/result.json").is_file())

    def test_timeout_kills_the_launched_process(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            with self.assertRaises(TimeoutError):
                evaluation.execute([sys.executable, "-c", "import time; time.sleep(3)"], root, 0.1)
            self.assertTrue((root / "stderr.log").is_file())


@unittest.skipUnless(len(NATIVE) == 3, "native executable, CLI fixture and library fixture required")
class NativeFlow(unittest.TestCase):
    def setUp(self):
        Corpus.setUp(self)

    def tearDown(self):
        Corpus.tearDown(self)

    def test_native_live_and_review_comparison_replay_and_baseline_exit_codes(self):
        binary, tool, library = NATIVE
        wav(self.source, seconds=21)
        self.reference.write_text("block 1 block 2", encoding="utf-8")
        Corpus.add(self)
        model = self.root / "fixture.gguf"
        model.write_bytes(b"fixture checkpoint identity")
        config = evaluation.read_json(self.corpus / "models.json")
        config["tools"] = {"fbdb": str(binary), "parakeet": str(tool), "library": str(library), "diar": str(model)}
        for variant in config["variants"]:
            variant["asr"] = str(model)
        config_path = self.corpus / "models.json"
        evaluation.write_json(config_path, config)
        first = self.root / "first"
        self.assertEqual(evaluation.run_comparison(self.corpus, config_path, first), 0)
        report = evaluation.read_json(first / "report.json")
        self.assertEqual([item["status"] for item in report["results"]], ["ok", "ok"])
        self.assertEqual(report["results"][0]["wer_percent"], 0)
        self.assertGreater(report["results"][1]["wer_percent"], 0)
        self.assertGreater(report["results"][0]["elapsed_seconds"], 0)
        if sys.platform in ("win32", "linux"):
            self.assertGreater(report["results"][0]["peak_resident_bytes"], 0)
        self.assertEqual(report["variants"][0]["files"]["asr"]["sha256"], evaluation.identity(model)["sha256"])
        self.assertEqual(evaluation.run_comparison(self.corpus, config_path, self.root / "repeated", first / "report.json"), 0)
        worse_baseline = copy.deepcopy(report)
        worse_baseline["results"][1]["words"]["errors"] = 0
        worse_baseline["results"][1]["wer_percent"] = 0
        evaluation.write_json(self.root / "zero-errors.json", worse_baseline)
        self.assertEqual(evaluation.run_comparison(self.corpus, config_path, self.root / "regression", self.root / "zero-errors.json"), 1)
        with self.assertRaises(FileExistsError):
            evaluation.run_comparison(self.corpus, config_path, first)


if __name__ == "__main__":
    unittest.main()
