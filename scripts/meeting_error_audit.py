"""Explain saved ASR word errors locally without running a model."""

import argparse
from collections import Counter
from pathlib import Path

import meeting_eval as evaluation


def align_words(reference, hypothesis):
    expected = evaluation.normalized(reference).split()
    actual = evaluation.normalized(hypothesis).split()
    distances = [[0] * (len(actual) + 1) for _ in range(len(expected) + 1)]
    for row in range(len(expected) + 1):
        distances[row][0] = row
    for column in range(len(actual) + 1):
        distances[0][column] = column
    for row, word in enumerate(expected, 1):
        for column, observed in enumerate(actual, 1):
            distances[row][column] = min(
                distances[row - 1][column - 1] + int(word != observed),
                distances[row - 1][column] + 1,
                distances[row][column - 1] + 1,
            )
    row, column = len(expected), len(actual)
    alignment = []
    while row or column:
        diagonal = None
        if row and column:
            diagonal = distances[row - 1][column - 1] + int(expected[row - 1] != actual[column - 1])
        if distances[row][column] == diagonal:
            row -= 1
            column -= 1
            operation = "match" if expected[row] == actual[column] else "substitution"
            entry = {"operation": operation, "reference": expected[row], "hypothesis": actual[column]}
        elif row and distances[row][column] == distances[row - 1][column] + 1:
            row -= 1
            entry = {"operation": "deletion", "reference": expected[row], "hypothesis": None}
        else:
            column -= 1
            entry = {"operation": "insertion", "reference": None, "hypothesis": actual[column]}
        alignment.append({**entry, "reference_index": row, "hypothesis_index": column})
    alignment.reverse()
    counts = Counter(entry["operation"] for entry in alignment)
    words = {
        "errors": counts["substitution"] + counts["deletion"] + counts["insertion"],
        "substitutions": counts["substitution"],
        "deletions": counts["deletion"],
        "insertions": counts["insertion"],
        "reference_units": len(expected),
    }
    if words != evaluation.edit_counts(expected, actual):
        raise ValueError("Alignment differs from the benchmark scorer")
    return alignment, words


def error_blocks(alignment, reference):
    blocks = []
    current = []
    expected = evaluation.normalized(reference).split()
    for entry in alignment + [{"operation": "match"}]:
        if entry["operation"] != "match":
            current.append(entry)
            continue
        if not current:
            continue
        start = current[0]["reference_index"]
        reference_words = [entry["reference"] for entry in current if entry["reference"] is not None]
        end = start + len(reference_words)
        hypothesis_words = [entry["hypothesis"] for entry in current if entry["hypothesis"] is not None]
        blocks.append({
            "reference_start": start,
            "reference_end": end,
            "reference": " ".join(reference_words),
            "hypothesis": " ".join(hypothesis_words),
            "operations": dict(Counter(entry["operation"] for entry in current)),
            "reference_context": " ".join(expected[max(0, start - 3):min(len(expected), end + 3)]),
        })
        current = []
    return blocks


def checked_text(record):
    actual = evaluation.identity(record["path"])
    if any(actual[key] != record[key] for key in ["sha256", "bytes"]):
        raise ValueError("Saved audit input changed: " + record["path"])
    return Path(record["path"]).read_text(encoding="utf-8-sig")


def audit(manifest):
    cases = {case["id"]: case for case in manifest["cases"]}
    references = {name: checked_text(case["reference"]) for name, case in cases.items()}
    if any(not evaluation.normalized(text) for text in references.values()):
        raise ValueError("Scored speech needs a checked, nonempty reference")
    models = []
    for model in manifest["models"]:
        if set(model["cases"]) != set(cases):
            raise ValueError("Every model must cover the same scored cases")
        results = []
        for case_id, source in model["cases"].items():
            hypothesis = checked_text(source["hypothesis"])
            alignment, words = align_words(references[case_id], hypothesis)
            if words != source["expected_words"]:
                raise ValueError("Word edits differ from the recorded benchmark: " + model["id"])
            results.append({
                "case_id": case_id,
                "words": words,
                "wer_percent": 100 * words["errors"] / words["reference_units"],
                "edits": [entry for entry in alignment if entry["operation"] != "match"],
                "blocks": error_blocks(alignment, references[case_id]),
            })
        totals = {key: sum(result["words"][key] for result in results) for key in results[0]["words"]}
        models.append({
            "id": model["id"],
            "label": model["label"],
            "primary": model["primary"],
            "words": totals,
            "wer_percent": 100 * totals["errors"] / totals["reference_units"],
            "results": results,
        })
    return {
        "schema_version": 1,
        "normalization": evaluation.NORMALIZATION,
        "cases": [{"id": name, "reference_words": len(evaluation.normalized(text).split())}
                  for name, text in references.items()],
        "alignment_policy": "Minimum word-edit distance; ties prefer substitution, deletion, insertion. Indices are zero-based.",
        "interpretation": "Differences from the frozen references, not independently verified acoustic mistakes.",
        "models": models,
    }


def cell(value):
    return (value or "∅").replace("|", "\\|").replace("\n", " ")


def markdown(report):
    reference_words = sum(case["reference_words"] for case in report["cases"])
    lines = ["# Differenze ASR per modello — materiale locale privato", "",
             f"Confronto con {len(report['cases'])} TXT congelati del benchmark: {reference_words} parole. Gli originali restano invariati.", "",
             "S = sostituzioni, D = omissioni, I = aggiunte. WER = (S+D+I)/parole del riferimento. Maiuscole e punteggiatura sono normalizzate; accenti, apostrofi interni e intercalari rimangono.", "",
             "Ogni blocco raggruppa edit contigui; il numero di righe può essere inferiore al numero di errori. Le colonne mostrano riferimento → modello. Le differenze non dimostrano da sole quale parola fosse pronunciata: controllare l’audio per i casi dubbi. Nessun errore viene escluso dal punteggio originale.", "",
             "| Modello | Profilo | S | D | I | Totale | WER |", "| --- | --- | --- | --- | --- | --- | --- |"]
    for model in report["models"]:
        words = model["words"]
        profile = "primario" if model["primary"] else "secondario/percorso app"
        lines.append(f"| {cell(model['label'])} | {profile} | {words['substitutions']} | {words['deletions']} | {words['insertions']} | {words['errors']} | {model['wer_percent']:.2f}% |")
    for model in report["models"]:
        lines += ["", f"## {model['label']} (`{model['id']}`)", ""]
        for result in model["results"]:
            words = result["words"]
            lines += [f"### {result['case_id']}", "",
                      f"WER {result['wer_percent']:.2f}% — S={words['substitutions']}, D={words['deletions']}, I={words['insertions']}; {words['reference_units']} parole.", "",
                      "| Riferimento | Modello | Edit | Contesto del riferimento |", "| --- | --- | --- | --- |"]
            for block in result["blocks"]:
                types = ", ".join(f"{name}={count}" for name, count in block["operations"].items())
                lines.append(f"| {cell(block['reference'])} | {cell(block['hypothesis'])} | {types} | {cell(block['reference_context'])} |")
    return "\n".join(lines) + "\n"


def write_audit(manifest_path, output):
    report = audit(evaluation.read_json(manifest_path))
    output.mkdir(parents=True, exist_ok=False)
    evaluation.write_json(output / "differences.json", report)
    (output / "differences.md").write_text(markdown(report), encoding="utf-8")
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, required=True, help="Local saved hypotheses, frozen references and expected word counts")
    parser.add_argument("--output", type=Path, required=True, help="New private output folder; reports contain transcript excerpts")
    options = parser.parse_args()
    report = write_audit(options.manifest, options.output)
    print(f"Verified {len(report['models'])} model profiles; private report: {options.output / 'differences.md'}")


if __name__ == "__main__":
    main()
