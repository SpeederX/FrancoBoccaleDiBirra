import itertools
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import meeting_error_audit as audit
import meeting_eval as evaluation


class ErrorAudit(unittest.TestCase):
    def test_alignment_reconstructs_inputs_and_matches_frozen_scorer_including_ties(self):
        sequences = [[]]
        for length in range(1, 4):
            sequences.extend(itertools.product(["a", "b"], repeat=length))
        for reference in sequences:
            for hypothesis in sequences:
                alignment, words = audit.align_words(" ".join(reference), " ".join(hypothesis))
                self.assertEqual([entry["reference"] for entry in alignment if entry["reference"] is not None], list(reference))
                self.assertEqual([entry["hypothesis"] for entry in alignment if entry["hypothesis"] is not None], list(hypothesis))
                self.assertEqual(words, evaluation.edit_counts(reference, hypothesis))
        _, words = audit.align_words("a b", "b a")
        self.assertEqual((words["substitutions"], words["deletions"], words["insertions"]), (2, 0, 0))

    def test_normalization_keeps_contractions_and_spoken_parenthetical_words(self):
        _, words = audit.align_words("Ciao, l’SDI (va bene)!", "ciao l'sdi va bene")
        self.assertEqual(words["errors"], 0)
        _, words = audit.align_words("c'è l'ade", "c'è la de")
        self.assertEqual(words["errors"], 2)

    def test_blocks_group_adjacent_edits_and_preserve_insertion_positions(self):
        reference = "ciao ho fatto la fattura"
        alignment, words = audit.align_words(reference, "ciao emetti la fattura oggi")
        blocks = audit.error_blocks(alignment, reference)
        self.assertEqual(len(blocks), 2)
        self.assertEqual(blocks[0]["reference"], "ho fatto")
        self.assertEqual(blocks[0]["hypothesis"], "emetti")
        self.assertEqual(blocks[1]["reference_start"], 5)
        self.assertEqual(blocks[1]["reference_end"], 5)
        self.assertEqual(blocks[1]["hypothesis"], "oggi")
        self.assertEqual(sum(sum(block["operations"].values()) for block in blocks), words["errors"])

    def test_private_report_uses_total_words_and_rejects_changed_inputs_or_counts(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            cases = []
            results = {}
            for index, (reference, hypothesis) in enumerate([("ciao come stai", "ciao tu stai"), ("bene", "")]):
                name = "case" + str(index)
                ref = root / (name + "-reference.txt")
                hyp = root / (name + "-hypothesis.txt")
                ref.write_text(reference, encoding="utf-8")
                hyp.write_text(hypothesis, encoding="utf-8")
                cases.append({"id": name, "reference": evaluation.identity(ref)})
                results[name] = {"hypothesis": evaluation.identity(hyp), "expected_words": evaluation.score(reference, hypothesis)["words"]}
            manifest = {"schema_version": 1, "cases": cases,
                        "models": [{"id": "fixture", "label": "Fixture", "primary": True, "cases": results}]}
            manifest_path = root / "manifest.json"
            evaluation.write_json(manifest_path, manifest)
            report = audit.write_audit(manifest_path, root / "output")
            self.assertEqual(report["models"][0]["wer_percent"], 50)
            self.assertIn("4 parole", (root / "output/differences.md").read_text(encoding="utf-8"))
            with self.assertRaises(FileExistsError):
                audit.write_audit(manifest_path, root / "output")
            results["case0"]["expected_words"]["errors"] = 0
            with self.assertRaisesRegex(ValueError, "recorded benchmark"):
                audit.audit(manifest)
            results["case0"]["expected_words"]["errors"] = 1
            Path(cases[0]["reference"]["path"]).write_text("changed", encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "input changed"):
                audit.audit(manifest)


if __name__ == "__main__":
    unittest.main()
