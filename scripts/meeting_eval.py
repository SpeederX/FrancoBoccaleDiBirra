"""Local, repeatable ASR comparisons against independently checked transcripts."""

import argparse
import ctypes
import datetime
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time
import unicodedata
import wave


PROJECT = Path(__file__).resolve().parents[1]
BINARY = PROJECT / "build/native/Release/fbdb.exe"
DEFAULT_CORPUS = BINARY.parent / "meetings/asr-tests"
NORMALIZATION = "nfkc-casefold-punctuation-v1"


def read_json(path):
    value = json.loads(Path(path).read_text(encoding="utf-8-sig"))
    if not isinstance(value, dict):
        raise ValueError("JSON documents must contain an object: " + str(path))
    return value


def write_json(path, value):
    Path(path).write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def identity(path):
    path = Path(path).resolve(strict=True)
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return {"path": str(path), "sha256": digest.hexdigest(), "bytes": path.stat().st_size}


def normalized(text):
    text = unicodedata.normalize("NFKC", text).casefold().translate(str.maketrans("’‘ʼ", "'''"))
    characters = []
    for character in text:
        if character == "'" or character.isalnum() or unicodedata.category(character).startswith("M"):
            characters.append(character)
        else:
            characters.append(" ")
    text = re.sub(r"(?<!\w)'|'(?!\w)", " ", "".join(characters))
    return " ".join(text.split())


def edit_counts(reference, hypothesis):
    # Ties prefer substitution, then deletion, then insertion for stable counts.
    previous = [(index, 0, 0, index) for index in range(len(hypothesis) + 1)]
    for row, expected in enumerate(reference, 1):
        current = [(row, 0, row, 0)]
        for column, actual in enumerate(hypothesis, 1):
            distance, substitutions, deletions, insertions = previous[column - 1]
            difference = int(expected != actual)
            diagonal = (distance + difference, substitutions + difference, deletions, insertions)
            distance, substitutions, deletions, insertions = previous[column]
            deletion = (distance + 1, substitutions, deletions + 1, insertions)
            distance, substitutions, deletions, insertions = current[column - 1]
            insertion = (distance + 1, substitutions, deletions, insertions + 1)
            current.append(min((diagonal, deletion, insertion), key=lambda item: item[0]))
        previous = current
    distance, substitutions, deletions, insertions = previous[-1]
    return {"errors": distance, "substitutions": substitutions, "deletions": deletions,
            "insertions": insertions, "reference_units": len(reference)}


def score(reference, hypothesis):
    reference = normalized(reference)
    hypothesis = normalized(hypothesis)
    words = edit_counts(reference.split(), hypothesis.split())
    characters = edit_counts(reference, hypothesis)
    return {"words": words, "characters": characters,
            "wer_percent": 100 * words["errors"] / words["reference_units"] if reference else None,
            "cer_percent": 100 * characters["errors"] / characters["reference_units"] if reference else None}


def audio_duration(path):
    with wave.open(str(path), "rb") as audio:
        if (audio.getnchannels(), audio.getframerate(), audio.getsampwidth(), audio.getcomptype()) != (1, 16000, 2, "NONE"):
            raise ValueError("samples must be mono 16 kHz PCM16 WAV; use a Meeting audio.wav")
        duration = audio.getnframes() / audio.getframerate()
    if duration <= 0:
        raise ValueError("sample audio has no frames")
    return duration


def valid_id(value):
    if not isinstance(value, str) or not re.fullmatch(r"[a-zA-Z0-9][a-zA-Z0-9_-]*", value):
        raise ValueError("IDs must contain letters, numbers, underscores or hyphens")
    return value


def corpus_file(corpus, name):
    path = (corpus / name).resolve(strict=True)
    if not path.is_relative_to(corpus.resolve()):
        raise ValueError("case paths must stay inside the corpus")
    return path


def load_cases(corpus):
    manifest = read_json(corpus / "cases.json")
    if manifest.get("schema_version") != 1 or not isinstance(manifest.get("cases"), list):
        raise ValueError("invalid cases.json schema")
    if not manifest["cases"]:
        raise ValueError("corpus has no cases; add an audio sample and checked reference first")
    result = []
    seen = set()
    for case in manifest["cases"]:
        case_id = valid_id(case["id"])
        if case_id in seen:
            raise ValueError("duplicate case ID: " + case_id)
        seen.add(case_id)
        audio = corpus_file(corpus, case["audio"])
        reference_path = corpus_file(corpus, case["reference"])
        reference = reference_path.read_text(encoding="utf-8-sig")
        kind = case.get("kind", "speech")
        if kind not in ("speech", "silence") or bool(normalized(reference)) != (kind == "speech"):
            raise ValueError("speech cases need nonempty reference text; silence cases need an empty reference")
        result.append({**case, "kind": kind, "audio": identity(audio),
                       "reference": identity(reference_path), "reference_text": reference,
                       "audio_seconds": audio_duration(audio)})
    return result


def init_corpus(corpus, executable, assets):
    if corpus.exists():
        raise ValueError("corpus already exists; keep its cases and references")
    corpus.mkdir(parents=True)
    write_json(corpus / "cases.json", {"schema_version": 1, "cases": []})
    tools = {"fbdb": str(executable.resolve()),
             "parakeet": str(assets / "parakeet-v0.6.1-bin-win-cpu-x64/parakeet-cli.exe"),
             "library": str(assets / "parakeet-v0.6.1-lib-win-cpu-x64/parakeet.dll"),
             "diar": str(assets / "parakeet-bundle-small.gguf")}
    asr = str(assets / "tdt-0.6b-v3-q8_0.gguf")
    write_json(corpus / "models.json", {"schema_version": 1, "tools": tools, "timeout_seconds": 1800,
               "variants": [{"id": "live-q8", "mode": "live", "asr": asr},
                            {"id": "review-q8", "mode": "review", "asr": asr}]})
    print("Corpus created: " + str(corpus))
    print("Add independently checked reference text; models.json compares the live and offline paths.")


def add_case(corpus, case_id, audio, reference, kind, tags):
    case_id = valid_id(case_id)
    manifest = read_json(corpus / "cases.json")
    if manifest.get("schema_version") != 1 or not isinstance(manifest.get("cases"), list):
        raise ValueError("invalid cases.json schema")
    if any(case["id"] == case_id for case in manifest["cases"]):
        raise ValueError("case already exists: " + case_id)
    audio_duration(audio)
    text = reference.read_text(encoding="utf-8-sig")
    if bool(normalized(text)) != (kind == "speech"):
        raise ValueError("speech needs checked reference text; use --kind silence for an empty reference")
    destination = corpus / "samples" / case_id
    destination.mkdir(parents=True, exist_ok=False)
    shutil.copyfile(audio, destination / "audio.wav")
    shutil.copyfile(reference, destination / "reference.txt")
    manifest["cases"].append({"id": case_id, "audio": f"samples/{case_id}/audio.wav",
                              "reference": f"samples/{case_id}/reference.txt", "language": "it",
                              "kind": kind, "tags": tags})
    write_json(corpus / "cases.json", manifest)
    print("Added case: " + case_id)


def load_models(path):
    config = read_json(path)
    if config.get("schema_version") != 1 or not isinstance(config.get("variants"), list) or not config["variants"]:
        raise ValueError("models.json needs schema_version 1 and at least one variant")
    timeout = config.get("timeout_seconds", 1800)
    if isinstance(timeout, bool) or not isinstance(timeout, (int, float)) or not math.isfinite(timeout) or timeout <= 0:
        raise ValueError("timeout_seconds must be a finite positive number")
    tools = config["tools"]
    variants = []
    seen = set()
    for variant in config["variants"]:
        name = valid_id(variant["id"])
        if name in seen or variant["mode"] not in ("live", "review"):
            raise ValueError("duplicate variant ID or unsupported mode")
        seen.add(name)
        keys = ["fbdb", "library", "diar"] if variant["mode"] == "live" else ["parakeet", "diar"]
        files = {key: identity(Path(tools[key])) for key in keys}
        files["asr"] = identity(Path(variant["asr"]))
        variants.append({"id": name, "mode": variant["mode"], "files": files})
    return variants, timeout


def peak_memory(process):
    """Peak resident memory of the single ASR process; no child inference is launched."""
    if sys.platform == "win32":
        from ctypes import wintypes

        class Counters(ctypes.Structure):
            _fields_ = [("cb", wintypes.DWORD), ("faults", wintypes.DWORD)] + [
                (name, ctypes.c_size_t) for name in
                ("peak_working_set", "working_set", "peak_paged", "paged", "peak_nonpaged",
                 "nonpaged", "pagefile", "peak_pagefile")]

        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        psapi = ctypes.WinDLL("psapi", use_last_error=True)
        kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
        kernel.OpenProcess.restype = wintypes.HANDLE
        kernel.CloseHandle.argtypes = [wintypes.HANDLE]
        psapi.GetProcessMemoryInfo.argtypes = [wintypes.HANDLE, ctypes.POINTER(Counters), wintypes.DWORD]
        handle = kernel.OpenProcess(0x0410, False, process.pid)
        if not handle:
            return None
        try:
            counters = Counters()
            counters.cb = ctypes.sizeof(counters)
            if psapi.GetProcessMemoryInfo(handle, ctypes.byref(counters), counters.cb):
                return counters.peak_working_set
        finally:
            kernel.CloseHandle(handle)
    elif sys.platform.startswith("linux"):
        try:
            status = Path(f"/proc/{process.pid}/status").read_text()
        except FileNotFoundError:
            return None
        match = re.search(r"^VmHWM:\s+(\d+)\s+kB", status, re.MULTILINE)
        if match:
            return int(match[1]) * 1024
    return None


def execute(command, directory, timeout):
    started = time.perf_counter()
    peak = None
    with (directory / "stdout.log").open("wb") as stdout, (directory / "stderr.log").open("wb") as stderr:
        options = {"creationflags": subprocess.CREATE_NO_WINDOW} if sys.platform == "win32" else {}
        process = subprocess.Popen(command, stdout=stdout, stderr=stderr, stdin=subprocess.DEVNULL, **options)
        try:
            while True:
                observed = peak_memory(process)
                if observed is not None:
                    peak = max(peak or 0, observed)
                if process.poll() is not None:
                    break
                if time.perf_counter() - started >= timeout:
                    raise TimeoutError("ASR exceeded timeout_seconds")
                time.sleep(0.05)
        finally:
            if process.poll() is None:
                process.kill()
            process.wait()
    return process.returncode, time.perf_counter() - started, peak


def transcript_text(path, json_lines):
    text = path.read_text(encoding="utf-8-sig")
    updates = [json.loads(line) for line in text.splitlines() if line.strip()] if json_lines else [json.loads(text)]
    if not updates:
        raise ValueError("ASR produced no JSON output")
    utterances = []
    for update in updates:
        if not isinstance(update, dict) or not isinstance(update.get("utterances"), list):
            raise ValueError("ASR output has no utterances array")
        for utterance in update["utterances"]:
            if not isinstance(utterance, dict) or not isinstance(utterance.get("text"), str):
                raise ValueError("ASR utterance has no text")
            utterances.append(utterance["text"])
    return "\n".join(utterances)


def run_case(case, variant, directory, timeout):
    directory.mkdir(parents=True)
    result = {"case_id": case["id"], "variant_id": variant["id"], "status": "error",
              "audio": case["audio"], "reference": case["reference"], "kind": case["kind"],
              "audio_seconds": case["audio_seconds"]}
    files = {key: value["path"] for key, value in variant["files"].items()}
    if variant["mode"] == "live":
        shutil.copyfile(case["audio"]["path"], directory / "audio.wav")
        command = [files["fbdb"], "--meeting-transcribe", "--directory", str(directory),
                   "--library", files["library"], "--asr", files["asr"], "--diar", files["diar"]]
        transcript = directory / "transcript.json"
    else:
        command = [files["parakeet"], "scene", "--model", files["asr"], "--diar", files["diar"],
                   "--input", case["audio"]["path"], "--json"]
        transcript = directory / "stdout.log"
    result["command"] = command
    try:
        code, elapsed, memory = execute(command, directory, timeout)
        result.update({"exit_code": code, "elapsed_seconds": elapsed,
                       "real_time_factor": elapsed / case["audio_seconds"],
                       "peak_resident_bytes": memory, "memory_scope": "single_asr_process"})
        if code != 0:
            raise ValueError(f"ASR exited with code {code}; see stderr.log")
        hypothesis = transcript_text(transcript, variant["mode"] == "review")
        (directory / "hypothesis.txt").write_text(hypothesis + "\n", encoding="utf-8")
        result.update(score(case["reference_text"], hypothesis))
        result["status"] = "ok"
    except (OSError, ValueError, TimeoutError) as error:
        result["error"] = str(error)
    write_json(directory / "result.json", result)
    return result


def compare_baseline(report, baseline):
    if baseline.get("schema_version") != 1 or baseline.get("normalization") != report["normalization"]:
        raise ValueError("baseline schema or normalization differs")
    previous = {(item["case_id"], item["variant_id"]): item for item in baseline["results"]}
    comparisons = []
    for item in report["results"]:
        old = previous.get((item["case_id"], item["variant_id"]))
        comparison = {"case_id": item["case_id"], "variant_id": item["variant_id"]}
        if old is None:
            comparison["status"] = "new_case_or_variant"
        elif any(old[key]["sha256"] != item[key]["sha256"] for key in ("audio", "reference")) or old["kind"] != item["kind"]:
            comparison["status"] = "changed_input_or_reference"
        elif old["status"] != "ok" or item["status"] != "ok":
            comparison["status"] = "asr_failure"
        else:
            comparison.update({"status": "comparable", "word_error_delta": item["words"]["errors"] - old["words"]["errors"],
                               "elapsed_seconds_delta": item["elapsed_seconds"] - old["elapsed_seconds"],
                               "wer_percentage_point_delta": item["wer_percent"] - old["wer_percent"] if item["wer_percent"] is not None else None})
        comparisons.append(comparison)
    for key in previous.keys() - {(item["case_id"], item["variant_id"]) for item in report["results"]}:
        comparisons.append({"case_id": key[0], "variant_id": key[1], "status": "missing_case_or_variant"})
    return comparisons


def summary(report):
    lines = ["# Meeting ASR comparison", "", "Reference: independently checked sample text. No LLM correction is scored.", "",
             "| Case | Variant | WER | CER | Time (s) | RTF | Peak RAM (MiB) |",
             "| --- | --- | --- | --- | --- | --- | --- |"]
    for item in report["results"]:
        if item["status"] != "ok":
            lines.append(f"| {item['case_id']} | {item['variant_id']} | ERROR | — | — | — | — |")
            continue
        wer = f"{item['wer_percent']:.2f}%" if item["wer_percent"] is not None else f"{item['words']['insertions']} extra words"
        cer = f"{item['cer_percent']:.2f}%" if item["cer_percent"] is not None else "—"
        memory = f"{item['peak_resident_bytes'] / 1048576:.1f}" if item["peak_resident_bytes"] is not None else "unavailable"
        lines.append(f"| {item['case_id']} | {item['variant_id']} | {wer} | {cer} | {item['elapsed_seconds']:.3f} | {item['real_time_factor']:.3f} | {memory} |")
    lines += ["", "RTF = processing time / audio duration, including model loading. RAM covers one ASR process; runs are sequential.",
              "Model/tool hashes and errors: report.json. This does not measure coexistence with an active agent."]
    if "baseline_comparisons" in report:
        lines += ["", "## Baseline comparison", ""]
        for item in report["baseline_comparisons"]:
            detail = f"word error delta {item['word_error_delta']:+d}" if item["status"] == "comparable" else item["status"]
            lines.append(f"- {item['case_id']} / {item['variant_id']}: {detail}")
    return "\n".join(lines) + "\n"


def run_comparison(corpus, config_path, output, baseline_path=None):
    cases = load_cases(corpus)
    variants, timeout = load_models(config_path)
    baseline = read_json(baseline_path) if baseline_path else None
    report = {"schema_version": 1, "normalization": NORMALIZATION,
              "created_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
              "platform": sys.platform, "variants": variants, "cases": cases, "results": []}
    if baseline is not None:
        compare_baseline(report, baseline)
    output.mkdir(parents=True, exist_ok=False)
    for variant in variants:
        for case in cases:
            print(f"Running {case['id']} / {variant['id']}", flush=True)
            result = run_case(case, variant, output / variant["id"] / case["id"], timeout)
            report["results"].append(result)
            write_json(output / "report.json", report)
    if baseline is not None:
        report["baseline_comparisons"] = compare_baseline(report, baseline)
        report["baseline"] = identity(baseline_path)
    write_json(output / "report.json", report)
    (output / "report.md").write_text(summary(report), encoding="utf-8")
    print("Results: " + str(output / "report.md"))
    failures = any(item["status"] != "ok" for item in report["results"])
    regression = any(item["status"] != "comparable" or item["word_error_delta"] > 0
                     for item in report.get("baseline_comparisons", []))
    return int(failures or regression)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="action", required=True)
    init = commands.add_parser("init", help="create a private corpus and live/offline model comparison")
    init.add_argument("--executable", type=Path, default=BINARY)
    local_appdata = Path(os.environ.get("LOCALAPPDATA", Path.home() / ".local/share"))
    init.add_argument("--assets", type=Path, default=local_appdata / "FrancoBoccaleDiBirra/external/meeting")
    add = commands.add_parser("add", help="copy a WAV and independently checked reference into the corpus")
    add.add_argument("--id", required=True)
    add.add_argument("--audio", type=Path, required=True)
    add.add_argument("--reference", type=Path, required=True)
    add.add_argument("--kind", choices=["speech", "silence"], default="speech")
    add.add_argument("--tag", action="append", default=[])
    run = commands.add_parser("run", help="run configured variants sequentially and optionally check a baseline")
    run.add_argument("--models", type=Path)
    run.add_argument("--output", type=Path)
    run.add_argument("--baseline", type=Path)
    for command in (init, add, run):
        command.add_argument("--corpus", type=Path, default=DEFAULT_CORPUS)
    args = parser.parse_args(argv)
    corpus = args.corpus.resolve()
    try:
        if args.action == "init":
            init_corpus(corpus, args.executable, args.assets.resolve())
        elif args.action == "add":
            add_case(corpus, args.id, args.audio, args.reference, args.kind, args.tag)
        else:
            stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ")
            output = (args.output or corpus / "runs" / stamp).resolve()
            return run_comparison(corpus, args.models or corpus / "models.json", output, args.baseline)
    except (OSError, ValueError, KeyError, TypeError, wave.Error) as error:
        print("Error: " + str(error), file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
