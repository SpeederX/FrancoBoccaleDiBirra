"""Headless local ASR benchmark; optional engines live in an isolated Python environment."""

import argparse
import ctypes
import importlib.metadata
import json
import os
from pathlib import Path
import sys
import threading
import time
from types import SimpleNamespace
import wave

import meeting_eval as evaluation


def read_audio(path):
    import numpy as np

    evaluation.audio_duration(path)
    with wave.open(str(path), "rb") as stream:
        return np.frombuffer(stream.readframes(stream.getnframes()), dtype="<i2").astype(np.float32) / 32768


def chunks(audio, seconds):
    frames = round(seconds * 16000)
    if frames <= 0:
        raise ValueError("chunk_seconds must be positive")
    for start in range(0, len(audio), frames):
        yield start / 16000, audio[start:start + frames]


def load_cases(path, requested):
    manifest = evaluation.read_json(path)
    if manifest.get("schema_version") != 1 or not manifest.get("cases"):
        raise ValueError("corpus must contain cases")
    seen = set()
    cases = []
    for case in manifest["cases"]:
        name = evaluation.valid_id(case["id"])
        if name in seen:
            raise ValueError("duplicate case: " + name)
        seen.add(name)
        if requested and name not in requested:
            continue
        if case["language"] not in ("it", "en"):
            raise ValueError("benchmark supports Italian and English")
        for key in ("audio", "reference"):
            actual = evaluation.identity(case[key]["path"])
            if actual["sha256"] != case[key]["sha256"]:
                raise ValueError("corpus file changed: " + actual["path"])
        reference = Path(case["reference"]["path"]).read_text(encoding="utf-8-sig")
        if case["scored"] and case["group"] != "negative-controls" and not evaluation.normalized(reference):
            raise ValueError("scored speech needs a nonempty reference")
        duration = evaluation.audio_duration(case["audio"]["path"])
        cases.append({**case, "reference_text": reference, "audio_seconds": duration})
    if requested and set(requested) - seen:
        raise ValueError("requested case is not in the corpus")
    return cases


class Monitor:
    def __init__(self):
        self.process = SimpleNamespace(pid=os.getpid())
        self.peak_ram = 0
        self.gpu_baseline = None
        self.gpu_peak = None
        self.done = threading.Event()
        try:
            import pynvml

            pynvml.nvmlInit()
            self.nvml = pynvml
            self.device = pynvml.nvmlDeviceGetHandleByIndex(0)
            self.gpu_baseline = pynvml.nvmlDeviceGetMemoryInfo(self.device).used
            self.gpu_peak = self.gpu_baseline
        except (ImportError, OSError):
            self.nvml = None
        self.thread = threading.Thread(target=self.sample, daemon=True)

    def sample(self):
        while not self.done.is_set():
            resident = evaluation.peak_memory(self.process)
            if resident is not None:
                self.peak_ram = max(self.peak_ram, resident)
            if self.nvml:
                observed = self.nvml.nvmlDeviceGetMemoryInfo(self.device).used
                self.gpu_peak = max(self.gpu_peak, observed)
            self.done.wait(0.1)

    def start(self):
        self.thread.start()

    def finish(self):
        self.done.set()
        self.thread.join()
        if self.nvml:
            self.nvml.nvmlShutdown()
        return {"peak_resident_bytes": self.peak_ram, "gpu_baseline_bytes": self.gpu_baseline,
                "gpu_peak_total_bytes": self.gpu_peak,
                "gpu_peak_increment_bytes": self.gpu_peak - self.gpu_baseline if self.gpu_peak is not None else None,
                "memory_scope": "ASR Python process lifetime RAM; device-wide VRAM sampled every 100 ms"}


class Parakeet:
    def __init__(self, variant):
        self.lib = ctypes.CDLL(variant["library"])
        self.kernel32 = None
        if sys.platform == "win32":
            self.kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
            self.kernel32.GetModuleHandleA.argtypes = [ctypes.c_char_p]
            self.kernel32.GetModuleHandleA.restype = ctypes.c_void_p
            self.kernel32.GetModuleHandleExA.argtypes = [ctypes.c_uint32, ctypes.c_void_p,
                                                       ctypes.POINTER(ctypes.c_void_p)]
            self.kernel32.GetModuleHandleExA.restype = ctypes.c_int
            self.kernel32.FreeLibrary.argtypes = [ctypes.c_void_p]
            self.kernel32.FreeLibrary.restype = ctypes.c_int
            openmp = self.kernel32.GetModuleHandleA(b"VCOMP140.DLL")
            if openmp:
                pinned = ctypes.c_void_p()
                if not self.kernel32.GetModuleHandleExA(1 | 4, openmp, ctypes.byref(pinned)):
                    raise ctypes.WinError(ctypes.get_last_error())
        signatures = {
            "parakeet_capi_abi_version": ([], ctypes.c_int),
            "parakeet_capi_load": ([ctypes.c_char_p], ctypes.c_void_p),
            "parakeet_capi_load_error": ([], ctypes.c_char_p),
            "parakeet_capi_last_error": ([ctypes.c_void_p], ctypes.c_char_p),
            "parakeet_capi_free": ([ctypes.c_void_p], None),
            "parakeet_capi_free_string": ([ctypes.c_void_p], None),
            "parakeet_capi_transcribe_pcm_lang": ([ctypes.c_void_p, ctypes.POINTER(ctypes.c_float), ctypes.c_int,
                                                   ctypes.c_int, ctypes.c_int, ctypes.c_char_p], ctypes.c_void_p),
        }
        for name, (arguments, result) in signatures.items():
            function = getattr(self.lib, name)
            function.argtypes = arguments
            function.restype = result
        if self.lib.parakeet_capi_abi_version() != 10:
            raise ValueError("requires the pinned Parakeet ABI 10 library")
        self.context = self.lib.parakeet_capi_load(os.fsencode(variant["model"]))
        if not self.context:
            raise ValueError(self.lib.parakeet_capi_load_error().decode("utf-8"))
        self.chunk_seconds = variant.get("chunk_seconds", 30)

    def transcribe(self, audio, language):
        segments = []
        for offset, chunk in chunks(audio, self.chunk_seconds):
            pointer = chunk.ctypes.data_as(ctypes.POINTER(ctypes.c_float))
            output = self.lib.parakeet_capi_transcribe_pcm_lang(self.context, pointer, len(chunk), 16000, 0, language.encode("ascii"))
            if not output:
                raise ValueError(self.lib.parakeet_capi_last_error(self.context).decode("utf-8"))
            try:
                text = ctypes.string_at(output).decode("utf-8")
            finally:
                self.lib.parakeet_capi_free_string(output)
            segments.append({"start": offset, "end": offset + len(chunk) / 16000, "text": text})
        return segments

    def close(self):
        self.lib.parakeet_capi_free(self.context)
        if self.kernel32:
            # Release CUDA globals before process shutdown tears down the CUDA driver.
            if not self.kernel32.FreeLibrary(self.lib._handle):
                raise ctypes.WinError(ctypes.get_last_error())


class Whisper:
    def __init__(self, variant):
        # Torch's Windows wheel supplies cuBLAS/cuDNN; register only this process's DLL path.
        import importlib.util

        spec = importlib.util.find_spec("torch")
        self.dll_directory = None
        if sys.platform == "win32" and spec:
            directory = Path(spec.origin).parent / "lib"
            self.dll_directory = os.add_dll_directory(str(directory))
            os.environ["PATH"] = str(directory) + os.pathsep + os.environ["PATH"]
        from faster_whisper import WhisperModel

        self.variant = variant
        self.model = WhisperModel(variant["model"], device=variant.get("device", "cuda"),
                                  compute_type=variant.get("compute_type", "float16"),
                                  cpu_threads=6, num_workers=1, local_files_only=True)

    def transcribe(self, audio, language):
        segments, _ = self.model.transcribe(audio, language=language, task="transcribe", beam_size=5,
                                            temperature=0, condition_on_previous_text=False,
                                            vad_filter=False, word_timestamps=False)
        return [{"start": item.start, "end": item.end, "text": item.text} for item in segments]

    def close(self):
        del self.model
        if self.dll_directory:
            self.dll_directory.close()


class Qwen:
    def __init__(self, variant):
        import torch
        from qwen_asr import Qwen3ASRModel

        torch.set_num_threads(6)
        self.torch = torch
        self.chunk_seconds = variant.get("chunk_seconds", 30)
        self.model = Qwen3ASRModel.from_pretrained(variant["model"], dtype=torch.float16,
                                                 device_map="cuda:0", attn_implementation="eager",
                                                 max_inference_batch_size=1, max_new_tokens=512)

    def transcribe(self, audio, language):
        segments = []
        language = {"it": "Italian", "en": "English"}[language]
        for offset, chunk in chunks(audio, self.chunk_seconds):
            result = self.model.transcribe(audio=(chunk, 16000), language=language)[0]
            segments.append({"start": offset, "end": offset + len(chunk) / 16000, "text": result.text})
        self.torch.cuda.synchronize()
        return segments

    def close(self):
        del self.model
        self.torch.cuda.empty_cache()


class Canary:
    def __init__(self, variant):
        import torch
        from transformers import AutoModelForSpeechSeq2Seq, AutoProcessor

        torch.set_num_threads(6)
        self.torch = torch
        self.chunk_seconds = variant.get("chunk_seconds", 30)
        self.processor = AutoProcessor.from_pretrained(variant["model"], local_files_only=True)
        self.model = AutoModelForSpeechSeq2Seq.from_pretrained(variant["model"], dtype=torch.float16,
                                                              device_map="cuda:0", attn_implementation="sdpa",
                                                              local_files_only=True)
        self.model.eval()

    def transcribe(self, audio, language):
        segments = []
        for offset, chunk in chunks(audio, self.chunk_seconds):
            inputs = self.processor.apply_transcription_request(audio=chunk, source_language=language)
            inputs = inputs.to(self.model.device, dtype=self.torch.float16)
            with self.torch.inference_mode():
                generated = self.model.generate(**inputs, max_new_tokens=512, do_sample=False, num_beams=1)
            decoded = self.processor.decode(generated, skip_special_tokens=True)
            text = decoded[0] if isinstance(decoded, list) else decoded
            segments.append({"start": offset, "end": offset + len(chunk) / 16000, "text": text})
        self.torch.cuda.synchronize()
        return segments

    def close(self):
        del self.model
        self.torch.cuda.empty_cache()


ENGINES = {"parakeet": Parakeet, "whisper": Whisper, "qwen": Qwen, "canary": Canary}


def aggregate(results):
    groups = {}
    for result in results:
        group = result["group"]
        item = groups.setdefault(group, {"cases": 0, "failures": 0, "reference_words": 0, "word_errors": 0,
                                         "reference_characters": 0, "character_errors": 0,
                                         "audio_seconds": 0, "inference_seconds": 0})
        if result["status"] != "ok":
            item["failures"] += 1
            continue
        item["cases"] += 1
        item["audio_seconds"] += result["audio_seconds"]
        item["inference_seconds"] += result["inference_seconds"]
        if result["scored"]:
            item["reference_words"] += result["words"]["reference_units"]
            item["word_errors"] += result["words"]["errors"]
            item["reference_characters"] += result["characters"]["reference_units"]
            item["character_errors"] += result["characters"]["errors"]
    for item in groups.values():
        item["wer_percent"] = 100 * item["word_errors"] / item["reference_words"] if item["reference_words"] else None
        item["cer_percent"] = 100 * item["character_errors"] / item["reference_characters"] if item["reference_characters"] else None
        item["real_time_factor"] = item["inference_seconds"] / item["audio_seconds"] if item["audio_seconds"] else None
    return groups


def run(config_path, variant_id, corpus_path, output, requested):
    config = evaluation.read_json(config_path)
    matches = [item for item in config["variants"] if item["id"] == variant_id]
    if len(matches) != 1:
        raise ValueError("variant must occur exactly once")
    variant = matches[0]
    if variant["engine"] not in ENGINES:
        raise ValueError("unsupported ASR engine")
    cases = load_cases(corpus_path, requested)
    model_path = Path(variant["model"])
    model_files = [model_path] if model_path.is_file() else sorted(path for path in model_path.iterdir() if path.is_file())
    identities = [evaluation.identity(path) for path in model_files]
    output.mkdir(parents=True, exist_ok=False)
    report = {"schema_version": 1, "normalization": evaluation.NORMALIZATION, "variant": variant,
              "model_files": identities, "runner": evaluation.identity(__file__),
              "scorer": evaluation.identity(evaluation.__file__),
              "python": sys.version, "python_executable": sys.executable,
              "corpus": evaluation.identity(corpus_path), "results": [], "complete": False}
    if "library" in variant:
        report["library"] = evaluation.identity(variant["library"])
    report["packages"] = {}
    for package in ("numpy", "torch", "qwen-asr", "transformers", "faster-whisper", "ctranslate2", "nvidia-ml-py"):
        try:
            report["packages"][package] = importlib.metadata.version(package)
        except importlib.metadata.PackageNotFoundError:
            continue
    monitor = Monitor()
    monitor.start()
    model = None
    try:
        started = time.perf_counter()
        model = ENGINES[variant["engine"]](variant)
        report["load_seconds"] = time.perf_counter() - started
        print("Loaded", variant_id, "in", round(report["load_seconds"], 2), "seconds", flush=True)
        for index, case in enumerate(cases, 1):
            result = {key: value for key, value in case.items() if key != "reference_text"}
            directory = output / case["id"]
            directory.mkdir()
            try:
                audio = read_audio(case["audio"]["path"])
                started = time.perf_counter()
                segments = model.transcribe(audio, case["language"])
                elapsed = time.perf_counter() - started
                hypothesis = "\n".join(item["text"] for item in segments)
                (directory / "hypothesis.txt").write_text(hypothesis + "\n", encoding="utf-8")
                evaluation.write_json(directory / "segments.json", {"segments": segments})
                result.update({"status": "ok", "inference_seconds": elapsed, "real_time_factor": elapsed / case["audio_seconds"]})
                if case["scored"]:
                    result.update(evaluation.score(case["reference_text"], hypothesis))
            except Exception as error:
                result.update({"status": "error", "error": type(error).__name__ + ": " + str(error)})
            report["results"].append(result)
            evaluation.write_json(directory / "result.json", result)
            report["groups"] = aggregate(report["results"])
            evaluation.write_json(output / "report.json", report)
            print(f"{variant_id} {index}/{len(cases)} {case['id']} {result['status']} WER={result.get('wer_percent')} time={result.get('inference_seconds')}", flush=True)
        report["complete"] = True
    finally:
        if model:
            model.close()
        report["memory"] = monitor.finish()
        evaluation.write_json(output / "report.json", report)
    return int(any(item["status"] != "ok" for item in report["results"]))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", required=True, type=Path)
    parser.add_argument("--variant", required=True)
    parser.add_argument("--corpus", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--case", action="append", default=[])
    args = parser.parse_args()
    return run(args.config, args.variant, args.corpus, args.output, args.case)


if __name__ == "__main__":
    sys.exit(main())
