import importlib.util
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock
import wave


SCRIPTS = Path(__file__).resolve().parents[1] / "scripts"
sys.path.insert(0, str(SCRIPTS))
import meeting_benchmark as benchmark
import meeting_eval as evaluation


class Benchmark(unittest.TestCase):
    def test_groups_use_total_errors_and_words_instead_of_average_clip_wer(self):
        first = {"group": "italian", "status": "ok", "audio_seconds": 10, "inference_seconds": 2,
                 "scored": True, "words": {"reference_units": 10, "errors": 5},
                 "characters": {"reference_units": 50, "errors": 10}}
        second = {**first, "audio_seconds": 20, "inference_seconds": 4,
                  "words": {"reference_units": 90, "errors": 0},
                  "characters": {"reference_units": 450, "errors": 0}}
        result = benchmark.aggregate([first, second])["italian"]
        self.assertEqual(result["wer_percent"], 5)
        self.assertEqual(result["cer_percent"], 2)
        self.assertEqual(result["real_time_factor"], 0.2)

    def test_failures_and_unreviewed_audio_cannot_improve_a_word_score(self):
        failed = {"group": "italian", "status": "error"}
        unreviewed = {"group": "unreviewed", "status": "ok", "scored": False,
                      "audio_seconds": 600, "inference_seconds": 30}
        result = benchmark.aggregate([failed, unreviewed])
        self.assertEqual(result["italian"]["failures"], 1)
        self.assertIsNone(result["italian"]["wer_percent"])
        self.assertIsNone(result["unreviewed"]["wer_percent"])
        self.assertEqual(result["unreviewed"]["real_time_factor"], 0.05)

    def test_empty_controls_report_insertions_without_wer(self):
        extra = evaluation.score("", "grazie per la visione")
        result = benchmark.aggregate([{"group": "negative-controls", "status": "ok", "scored": True,
                                      "audio_seconds": 10, "inference_seconds": 1, **extra}])
        self.assertIsNone(result["negative-controls"]["wer_percent"])
        self.assertEqual(result["negative-controls"]["word_errors"], 4)

    def test_frozen_corpus_rejects_changed_references_unknown_cases_and_unscored_speech(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            audio = root / "audio.wav"
            with wave.open(str(audio), "wb") as stream:
                stream.setparams((1, 2, 16000, 0, "NONE", "not compressed"))
                stream.writeframes(bytes(32000))
            reference = root / "reference.txt"
            reference.write_text("ciao", encoding="utf-8")
            case = {"id": "sample", "audio": evaluation.identity(audio), "reference": evaluation.identity(reference),
                    "language": "it", "group": "personal-it", "scored": True}
            manifest = root / "cases.json"
            evaluation.write_json(manifest, {"schema_version": 1, "cases": [case]})
            self.assertEqual(benchmark.load_cases(manifest, ["sample"])[0]["audio_seconds"], 1)
            with self.assertRaisesRegex(ValueError, "not in"):
                benchmark.load_cases(manifest, ["missing"])
            reference.write_text("", encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "changed"):
                benchmark.load_cases(manifest, [])
            case["reference"] = evaluation.identity(reference)
            evaluation.write_json(manifest, {"schema_version": 1, "cases": [case]})
            with self.assertRaisesRegex(ValueError, "nonempty"):
                benchmark.load_cases(manifest, [])
            case["scored"] = False
            evaluation.write_json(manifest, {"schema_version": 1, "cases": [case]})
            self.assertEqual(len(benchmark.load_cases(manifest, [])), 1)

    @unittest.skipUnless(importlib.util.find_spec("numpy"), "NumPy required for PCM checks")
    def test_runner_feeds_only_pcm_and_language_to_engine_and_preserves_input_bytes(self):
        import numpy as np

        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            audio = root / "audio.wav"
            with wave.open(str(audio), "wb") as stream:
                stream.setparams((1, 2, 16000, 0, "NONE", "not compressed"))
                stream.writeframes(bytes(32000))
            reference = root / "reference.txt"
            reference.write_text("ciao", encoding="utf-8")
            model = root / "checkpoint.bin"
            model.write_bytes(b"test-only model identity")
            corpus = root / "cases.json"
            evaluation.write_json(corpus, {"schema_version": 1, "cases": [{"id": "sample", "audio": evaluation.identity(audio),
                "reference": evaluation.identity(reference), "language": "it", "group": "test", "scored": True}]})
            config = root / "models.json"
            evaluation.write_json(config, {"variants": [{"id": "fixture", "engine": "whisper", "model": str(model)}]})
            fake = mock.Mock()
            fake.transcribe.return_value = [{"start": 0, "end": 1, "text": "ciao"}]
            before = audio.read_bytes(), reference.read_bytes()
            with mock.patch.dict(benchmark.ENGINES, {"whisper": mock.Mock(return_value=fake)}):
                self.assertEqual(benchmark.run(config, "fixture", corpus, root / "output", []), 0)
            pcm, language = fake.transcribe.call_args.args
            self.assertIsInstance(pcm, np.ndarray)
            self.assertEqual(language, "it")
            report = evaluation.read_json(root / "output/report.json")
            self.assertTrue(report["complete"])
            self.assertEqual(report["results"][0]["wer_percent"], 0)
            self.assertEqual(before, (audio.read_bytes(), reference.read_bytes()))
            fake.close.assert_called_once()
            with self.assertRaises(FileExistsError):
                benchmark.run(config, "fixture", corpus, root / "output", [])

    @unittest.skipUnless(importlib.util.find_spec("numpy"), "NumPy required for PCM checks")
    def test_chunks_preserve_all_audio_frames_and_reject_zero_length_configuration(self):
        import numpy as np

        audio = np.arange(320001, dtype=np.float32)
        parts = list(benchmark.chunks(audio, 20))
        self.assertEqual([offset for offset, _ in parts], [0, 20])
        self.assertTrue(np.array_equal(np.concatenate([part for _, part in parts]), audio))
        with self.assertRaisesRegex(ValueError, "positive"):
            list(benchmark.chunks(audio, 0))


if __name__ == "__main__":
    unittest.main()
