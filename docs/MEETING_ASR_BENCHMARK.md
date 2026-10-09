# Confronto locale fra motori ASR

`scripts/meeting_benchmark.py` confronta i modelli senza aprire Franco o
registrare il microfono. Usa gli stessi WAV PCM16 mono a 16 kHz e riferimenti
verificati, conserva le trascrizioni grezze e riutilizza il modello caricato per
tutti i campioni. Il riferimento viene usato solo dal calcolo degli errori,
dopo l'inferenza. Audio e testi vengono elaborati localmente.

## Motori e ambiente

- `parakeet`: libreria CPU di parakeet.cpp v0.6.1, C ABI 10, più NumPy.
- `whisper`: faster-whisper/CTranslate2; CPU INT8 oppure CUDA FP16/INT8-FP16.
- `qwen`: pacchetto ufficiale `qwen-asr`, CUDA FP16, attenzione eager.
- `canary`: Transformers con supporto nativo Canary, CUDA FP16, attenzione SDPA.

I pacchetti di Qwen e Canary possono richiedere versioni diverse di
Transformers: usare ambienti virtuali separati. I pacchetti e i modelli vanno
preparati prima del confronto; fornire percorsi locali dei checkpoint. Su
Windows il runner Whisper cerca le DLL CUDA nella cartella `torch/lib`
dell'ambiente virtuale. Versioni, hash, configurazione e identità del runner
sono conservati nel rapporto.

## Manifest del corpus

Il JSON ha `schema_version: 1` e una lista `cases`. Ogni elemento contiene:

- `id`: identificativo unico;
- `audio` e `reference`: oggetti con `path`, `sha256` e `bytes`;
- `language`: `it` o `en`;
- `group`: nome del gruppo da confrontare separatamente;
- `scored`: `true` per riferimenti verificati, `false` per prove senza riferimento.

Gli hash si ottengono con `meeting_eval.identity(path)`. Prima di iniziare il
runner verifica nuovamente audio e riferimenti. Una reference non verificata
non deve essere usata come ground truth. I controlli privi di parlato hanno
reference vuota e gruppo `negative-controls`; misurano le parole aggiunte.
Rimuovere dal riferimento le sole annotazioni dello speaker che non sono state
pronunciate, conservando anche l'originale e descrivendo questa trasformazione.

## Configurazione e avvio

Esempio di `models.json`:

```json
{
  "schema_version": 1,
  "variants": [
    {
      "id": "whisper-turbo-cuda",
      "engine": "whisper",
      "model": "D:/franco-asr/models/whisper-turbo",
      "device": "cuda",
      "compute_type": "float16"
    },
    {
      "id": "parakeet-q8-cpu",
      "engine": "parakeet",
      "model": "D:/franco-asr/models/tdt-0.6b-v3-q8_0.gguf",
      "library": "D:/franco-asr/runtime/parakeet.dll",
      "chunk_seconds": 30
    }
  ]
}
```

```powershell
python scripts/meeting_benchmark.py --config D:/franco-asr/models.json --variant whisper-turbo-cuda --corpus D:/franco-asr/cases.json --output D:/franco-asr/runs/turbo-01
```

La cartella di output deve essere nuova. `--case <id>` può essere ripetuto per
un controllo iniziale o un confronto mirato. Eseguire un solo modello alla
volta. Il runner salva un rapporto progressivo e i risultati di ogni campione;
`complete: true` indica che tutti i campioni selezionati sono stati attraversati.
Gli errori di inferenza rimangono risultati falliti, senza punteggi. Il processo
restituisce un codice diverso da zero in presenza di fallimenti.

## Misure e limiti

WER e CER usano la stessa normalizzazione di `meeting_eval.py`: NFKC,
minuscole, punteggiatura rimossa, apostrofi interni e accenti conservati. Numeri
e intercalari non vengono riscritti. Differenze ortografiche o di separazione
delle parole possono quindi contribuire al WER. Le percentuali aggregate
derivano dal totale degli errori diviso il totale delle parole/caratteri,
non dalla media dei WER dei singoli clip.

Il tempo di caricamento include gli import del motore. Gli hash dei pesi vengono
calcolati prima del caricamento: la cache dei file del sistema operativo può
essere calda. Il tempo per clip copre l'inferenza e il completamento CUDA,
escludendo lettura del WAV, scoring e scrittura dei risultati. RTF è il rapporto
fra tempo di inferenza e durata dell'audio: meno di 1 significa più veloce del
tempo reale, senza misurare la latenza dell'interfaccia di Meeting.

La RAM è il picco residente dell'intero processo Python, compresi import e
caricamento. La VRAM è il consumo dell'intera GPU campionato ogni 100 ms tramite
NVML: sono riportati valore iniziale, picco totale e incremento. L'incremento
è una stima del consumo ASR quando gli altri carichi restano stabili.
Non equivale a una prova di convivenza con un agent attivo.

Parakeet, Qwen e Canary usano blocchi consecutivi, senza sovrapposizione, della
durata indicata da `chunk_seconds`. Whisper usa la propria segmentazione.
Queste sono configurazioni del confronto: il percorso reale di Meeting usa
scene/diarizzazione e blocchi di input fino a 20 secondi. Per misurarlo usare
anche [meeting_eval.py](MEETING_ASR_TESTS.md). Il confronto fra ASR non misura
l'assegnazione dei nomi agli speaker o la qualità dei riepiloghi LLM.
