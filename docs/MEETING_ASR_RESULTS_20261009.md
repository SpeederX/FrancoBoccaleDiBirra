# Confronto ASR locale — 9 ottobre 2026

Confronto su questa macchina: Ryzen 5 2600, 6 core/12 thread, 32 GiB di RAM nominali, RTX 2070 da 8 GiB, Windows 11 Home, driver NVIDIA 610.88.

Sono state completate 15 configurazioni sull’identico corpus di 226 casi: 3390 risultati modello/campione. I due profili GPU INT8 sono verifiche secondarie: 6 casi privati/controlli e gli stessi 220 casi pubblici sono stati eseguiti in due processi. Gli hash degli input coincidono con quelli del confronto principale.

## Scelte pratiche su questa macchina

Per l’italiano quotidiano sceglierei Whisper large-v3-turbo FP16: 19.6 s per l’audio da 10 min 16 s, 1.70 GiB di RAM e 2.28 GiB di VRAM, con 31 errori sulle 270 parole personali contro i 28 di large-v3. È il compromesso più convincente fra qualità italiana e attesa in queste prove. Turbo INT8 riduce la VRAM a 1.28 GiB e mantiene gli stessi errori italiani, ma sui meeting AMI aggiunge 13 errori rispetto a FP16; usarlo quando il risparmio GPU conta più di quella differenza.

Per lasciare la GPU all’agent sceglierei la famiglia Parakeet CPU: Q4_K è il candidato più leggero, con 0.99 GiB di RAM, 101.7 s sull’audio lungo e risultati vicini al Q8. I due errori personali in meno non dimostrano una superiorità generale. Il Q8 attualmente integrato in Franco usa 1.24 GiB, completa l’audio lungo in 118.4 s e il percorso nativo a 20 s ottiene 12.59% WER sui due clip, includendo segmentazione/diarizzazione. La convivenza con un LLM caricato resta da misurare.

Per la revisione italiana finale sceglierei Whisper large-v3: FP16 ottiene il minor WER personale (10.37%) e FLEURS IT (1.65%). Richiede 66.7 s sul lungo, 3.07 GiB di RAM e 4.10 GiB di VRAM. INT8-FP16 conserva il WER personale e riduce la VRAM a 2.32 GiB, usando 3.32 GiB di RAM e 77.4 s; FLEURS IT passa a 1.73% e alcuni conteggi inglesi peggiorano leggermente. Questo è un candidato da scegliere per il secondo passaggio, non una revisione già attivata nell’app.

Per massima velocità ASR sceglierei Parakeet Q8 CUDA: 5.5 s sul lungo, 1.17 GiB di RAM e 1.19 GiB di VRAM. Sui meeting inglesi con microfono vicino è fra i migliori (6.75% WER), mentre sui due clip italiani resta dietro Whisper. La prima esecuzione GPU impiegava 47.45 s per caricamento + primo clip; la ripetizione 1.50 s. La prova riguarda il runner ASR da file: il percorso live di Franco su GPU non è stato qualificato.

Per lettura inglese Qwen3-ASR 1.7B ha il minor WER grezzo (4.11%), ma sul lungo richiede 196.8 s, 4.90 GiB di RAM e 4.59 GiB di VRAM. Per queste registrazioni italiane il costo non porta un vantaggio. Parakeet è più adatto ai meeting AMI; i risultati dipendono anche dal tipo di microfono. Il F16 Parakeet non migliora i due riferimenti personali rispetto al Q8 e aumenta memoria e attesa; non lo sceglierei come revisore sulla base di queste prove.

Non assegno un punteggio unico che mescoli lingue, generi, memoria e tempo: la classifica numerica seguente mostra la distanza dai riferimenti, mentre le scelte sopra indicano lo scenario d’uso. La valutazione personale è piccola; il lungo non ha una reference verificata.

## Qualità della trascrizione

Ordine: WER sui due riferimenti italiani indicati dall’utente come verificati; a pari WER, tempo sul campione lungo. Il rango riguarda questo piccolo insieme, non una superiorità universale. WER e CER minori indicano una minore distanza dal testo di riferimento.

| # | Configurazione | WER IT personale | CER IT personale | WER FLEURS IT | WER EN lettura | WER AMI cuffie | WER AMI distante |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 1 | Whisper large-v3 FP16 CUDA | 10.37% | 5.29% | 1.65% | 5.17% | 13.51% | 25.05% |
| 2 | Whisper large-v3 INT8-FP16 CUDA | 10.37% | 5.35% | 1.73% | 5.27% | 13.94% | 25.60% |
| 3 | Whisper large-v3-turbo FP16 CUDA | 11.48% | 6.08% | 2.28% | 5.12% | 16.34% | 28.20% |
| 4 | Whisper large-v3-turbo INT8-FP16 CUDA | 11.48% | 6.08% | 2.28% | 5.32% | 18.52% | 28.76% |
| 5 | Whisper medium FP16 CUDA | 11.48% | 4.92% | 3.77% | 6.42% | 18.95% | 28.20% |
| 6 | Parakeet TDT v3 Q8 CUDA | 13.70% | 7.11% | 2.12% | 5.62% | 6.75% | 19.67% |
| 7 | Parakeet TDT v3 Q4_K CPU | 13.70% | 6.87% | 2.35% | 5.77% | 7.63% | 17.07% |
| 8 | Parakeet TDT v3 Q8 CPU | 14.44% | 7.53% | 2.20% | 5.67% | 6.75% | 19.67% |
| 9 | Parakeet TDT v3 F16 CPU | 14.44% | 7.53% | 2.12% | 5.72% | 6.75% | 17.81% |
| 10 | Whisper small FP16 CUDA | 16.30% | 7.11% | 9.34% | 8.18% | 17.65% | 28.39% |
| 11 | Canary 1B v2 FP16 CUDA | 17.41% | 8.81% | 3.92% | 5.77% | 15.03% | 29.13% |
| 12 | Qwen3-ASR 1.7B FP16 CUDA | 17.41% | 10.27% | 2.59% | 4.11% | 9.37% | 21.52% |
| 13 | Nemotron 3.5 ASR Q8 CPU | 19.26% | 10.09% | 5.81% | 8.28% | 12.20% | 33.02% |
| 14 | Whisper base INT8 CPU | 20.37% | 9.60% | 15.62% | 17.46% | 46.19% | 91.09% |
| 15 | Qwen3-ASR 0.6B FP16 CUDA | 21.48% | 11.30% | 5.49% | 4.77% | 10.02% | 22.26% |

## Tempi e risorse

Audio lungo: 615.808 secondi (10 minuti e 16 secondi), privo di riferimento verificato. Si misurano inferenza e completamento delle chiamate, senza assegnargli un punteggio di accuratezza.

| # | Configurazione | Inferenza audio lungo s | RTF lungo | Load s | Primo clip + load s | RAM picco GiB | VRAM incremento GiB | Pesi su disco GB |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 1 | Whisper large-v3 FP16 CUDA | 66.7 | 0.108 | 9.2 | 15.00 | 3.07 | 4.10 | 3.09 |
| 2 | Whisper large-v3 INT8-FP16 CUDA | 77.4 | 0.126 | 8.8 | 15.14 | 3.32 | 2.32 | 3.09 |
| 3 | Whisper large-v3-turbo FP16 CUDA | 19.6 | 0.032 | 3.2 | 5.22 | 1.70 | 2.28 | 1.62 |
| 4 | Whisper large-v3-turbo INT8-FP16 CUDA | 21.2 | 0.034 | 4.6 | 6.62 | 1.96 | 1.28 | 1.62 |
| 5 | Whisper medium FP16 CUDA | 47.7 | 0.077 | 3.3 | 7.44 | 1.62 | 2.16 | 1.53 |
| 6 | Parakeet TDT v3 Q8 CUDA | 5.5 | 0.009 | 0.8 | 1.50 | 1.17 | 1.19 | 0.94 |
| 7 | Parakeet TDT v3 Q4_K CPU | 101.7 | 0.165 | 0.3 | 11.35 | 0.99 | 0.00 | 0.68 |
| 8 | Parakeet TDT v3 Q8 CPU | 118.4 | 0.192 | 0.4 | 13.20 | 1.24 | 0.00 | 0.94 |
| 9 | Parakeet TDT v3 F16 CPU | 141.3 | 0.229 | 0.8 | 16.07 | 1.71 | 0.00 | 1.44 |
| 10 | Whisper small FP16 CUDA | 29.7 | 0.048 | 1.5 | 4.96 | 1.13 | 0.78 | 0.48 |
| 11 | Canary 1B v2 FP16 CUDA | 31.4 | 0.051 | 35.7 | 41.44 | 2.61 | 2.08 | 3.92 |
| 12 | Qwen3-ASR 1.7B FP16 CUDA | 196.8 | 0.320 | 17.1 | 33.94 | 4.90 | 4.59 | 4.70 |
| 13 | Nemotron 3.5 ASR Q8 CPU | 138.7 | 0.225 | 0.3 | 15.46 | 1.30 | 0.00 | 0.98 |
| 14 | Whisper base INT8 CPU | 46.7 | 0.076 | 0.8 | 5.63 | 0.72 | 0.00 | 0.15 |
| 15 | Qwen3-ASR 0.6B FP16 CUDA | 182.6 | 0.297 | 36.1 | 52.76 | 2.72 | 2.06 | 1.88 |

RTF = tempo di inferenza / durata audio. Il modello è residente fra i campioni. Il caricamento include gli import del motore; gli hash dei pesi vengono letti prima e possono riscaldare la cache del sistema operativo. Parte dell’inizializzazione può ricadere nella prima inferenza. Nessun tempo qui misura la latenza della UI o dei dispositivi.

«Primo clip + load» somma caricamento e prima inferenza sullo stesso clip da 67.883 s: evidenzia le inizializzazioni differite, senza simulare la latenza live. Per Parakeet CUDA il primo tentativo richiese 47.45 s; la colonna usa la ripetizione successiva. Questa differenza fra avvii non è spiegata da una prova causale della cache o della compilazione dei kernel.

RAM: picco residente del processo ASR, comprendendo import e caricamento, massimo fra i passaggi del corpus. VRAM: massimo incremento del consumo dell’intera GPU rispetto al valore iniziale, campionato ogni 100 ms; è una stima con gli altri carichi GPU stabili. Le configurazioni CPU non allocano VRAM ASR. Non era caricato un modello LLM dell’agent: la convivenza reale non è stata misurata.

I profili Whisper INT8 riutilizzano gli stessi file CT2 su disco: la quantizzazione avviene nel runtime. I GiB di memoria usano 2³⁰ byte; i GB di file/download usano 10⁹ byte.

## Corpus e riferimenti

| Gruppo | Casi | Durata s | Parole riferimento |
| --- | --- | --- | --- |
| personal-it | 2 | 158.741 | 270 |
| unscored-long | 1 | 615.808 | 0 |
| unscored-short | 1 | 48.171 | 0 |
| negative-controls | 2 | 20.000 | 0 |
| fleurs-en | 50 | 486.840 | 1069 |
| fleurs-it | 50 | 820.860 | 1274 |
| librispeech-clean | 30 | 145.250 | 384 |
| librispeech-other | 30 | 199.690 | 540 |
| ami-ihm | 30 | 162.190 | 459 |
| ami-sdm | 30 | 167.760 | 539 |

I due campioni personali valutati hanno 270 parole e 158.741 secondi complessivi. Il file di riferimento del dialogo lungo risultava vuoto nella copia locale; l’audio breve aggiuntivo non ha una reference. Entrambi restano esclusi dai punteggi. Originali mantenuti; WAV di prova separati, PCM16 mono 16 kHz. Dai riferimenti vengono tolte soltanto le etichette dello speaker a inizio riga, che non sono parole pronunciate.

FLEURS: 50 ID distinti per lingua, estratti dal test con seed 20261009. LibriSpeech: 30 clip per split clean/other, selezionati con lo stesso seed. AMI: 10 turni con almeno 8 parole e almeno 3 secondi in ciascuna di tre pagine del test, per ciascun microfono, selezionati prima dell’inferenza su questi turni. Il subset AMI copre tre meeting. Nel microfono distante possono essere udibili voci sovrapposte oltre allo speaker annotato: il WER SDM è una prova difficile e non misura ogni parola aggiuntiva come errore acustico certo.

## Normalizzazione e controlli

WER/CER primari: NFKC, minuscole, punteggiatura rimossa, apostrofi interni e accenti conservati. I numeri, gli intercalari, le ripetizioni e tutte le parole tra parentesi rimangono. I punteggi aggregati sono errori totali / parole o caratteri totali, non la media dei WER dei clip.

Controllo inglese aggiuntivo: soltanto il dizionario ortografico britannico/americano di Whisper applicato ai token già normalizzati. Non si usa il normalizzatore completo, che eliminava alcune frasi tra parentesi nei riferimenti FLEURS. Il controllo non cambia il numero di parole del riferimento. I risultati per gruppo sono nel JSON.

AMI IHM: Whisper large-v3 ha 39 delezioni, delle quali 13 sono «uh/um»; Parakeet Q8 ha 5 delezioni. Parte della distanza riguarda gli intercalari. Questo WER misura fedeltà al riferimento, senza assegnare un punteggio alla leggibilità o alla correttezza semantica di un riepilogo.

| Configurazione | Parole aggiunte su silenzio 10 s | Parole aggiunte su rumore 10 s |
| --- | --- | --- |
| Whisper large-v3 FP16 CUDA | 2 | 2 |
| Whisper large-v3 INT8-FP16 CUDA | 2 | 2 |
| Whisper large-v3-turbo FP16 CUDA | 2 | 0 |
| Whisper large-v3-turbo INT8-FP16 CUDA | 2 | 1 |
| Whisper medium FP16 CUDA | 1 | 3 |
| Parakeet TDT v3 Q8 CUDA | 0 | 0 |
| Parakeet TDT v3 Q4_K CPU | 0 | 0 |
| Parakeet TDT v3 Q8 CPU | 0 | 0 |
| Parakeet TDT v3 F16 CPU | 0 | 0 |
| Whisper small FP16 CUDA | 1 | 0 |
| Canary 1B v2 FP16 CUDA | 0 | 0 |
| Qwen3-ASR 1.7B FP16 CUDA | 3 | 3 |
| Nemotron 3.5 ASR Q8 CPU | 0 | 0 |
| Whisper base INT8 CPU | 1 | 0 |
| Qwen3-ASR 0.6B FP16 CUDA | 1 | 1 |

Rumore: bianco gaussiano sintetico, ampiezza circa −50 dBFS, seed 20261009. VAD e filtri sulla confidenza non attivi nel confronto ASR. Due controlli sintetici non stimano la frequenza generale delle parole spurie nell’uso reale.

## Configurazioni e prove secondarie

Parakeet/Nemotron/Redux: parakeet.cpp v0.6.1, ABI 10, greedy, blocchi consecutivi di 30 secondi senza sovrapposizione, thread della libreria predefiniti. Parakeet CUDA usa la release ufficiale Windows con redistribuibili CUDA 12. Whisper: faster-whisper 1.2.1 / CTranslate2 4.8.2, beam 5, temperatura 0, lingua impostata, VAD disattivato, nessun contesto da segmenti precedenti, 6 thread CPU; segmentazione propria.

Qwen3-ASR: pacchetto 0.0.6, Transformers 4.57.6, Torch 2.8.0+cu126, CUDA FP16 eager, batch 1, max 512 token, blocchi 30 s, lingua impostata. Canary: Transformers 5.19.0 in ambiente separato, stesso Torch, CUDA FP16 SDPA, batch 1, greedy, max 512 token, blocchi 30 s. Python 3.13.13; NumPy 2.5.3; NVML 13.615.71. Conversione con FFmpeg 8.1.2.

Il costo è quello di questi runtime su questa GPU: non rappresenta il massimo ottenibile da implementazioni diverse o da GPU più nuove.

Redux packed CPU (213 MB) non entra nella classifica: il runtime v0.6.1 ha segnalato il kernel ternario scalare lento. Il primo audio da 67.9 s era ancora incompleto dopo 141.3 s dall’avvio del processo (attesa primo risultato/durata audio > 2.08; RAM osservata 0.68 GiB). Questo tempo comprende preparazione, hash dei pesi, caricamento e lettura WAV; è distinto dai tempi di inferenza dei modelli completati. Prova interrotta: nessun punteggio WER assegnato e nessun corpus completo dichiarato. Il risultato riguarda il file packed e questo runtime CPU; non qualifica le versioni Redux dequantizzate.

| Profilo secondario | WER IT personale | Delta errori vs base | Audio lungo s | RAM GiB | VRAM GiB |
| --- | --- | --- | --- | --- | --- |
| whisper-small-cpu-int8 | 16.30% | +0 | 149.5 | 0.89 | 0.00 |
| whisper-turbo-int8 | 11.48% | +0 | 21.2 | 1.96 | 1.28 |
| whisper-large-v3-int8 | 10.37% | +0 | 77.4 | 3.32 | 2.32 |
| parakeet-q8-chunk90 | 12.96% | -4 | 138.0 | 1.83 | 0.00 |
| qwen-1.7b-chunk90 | 16.30% | -3 | 182.0 | 4.90 | 5.38 |
| canary-1b-v2-chunk90 | 43.33% | +70 | 22.1 | 1.77 | 2.45 |

I due profili GPU INT8 sono verificati anche su tutti i 220 casi pubblici e compaiono nelle tabelle principali. Gli altri quattro profili sono ablazioni sui soli sei casi personali/controlli. Nei blocchi da 90 s di Qwen e Canary resta il limite di 512 token: un eventuale vantaggio di contesto va distinto dagli effetti di tagli e limiti di generazione. Canary peggiora con 90 s: non è stato identificato un taglio predefinito nel preprocessore e la causa non è dimostrata; conservare la configurazione da 30 s. Il lungo senza reference non dimostra un miglioramento di accuratezza.

## Percorso reale di Franco

| Percorso CPU Q8 | WER IT personale | CER IT personale | Tempo totale su due clip s | RAM GiB |
| --- | --- | --- | --- | --- |
| live-q8 | 12.59% | 6.32% | 41.9 | 1.23 |
| review-q8 | 12.22% | 6.26% | 42.0 | 1.19 |

`live-q8` usa il percorso nativo di Franco con blocchi di input fino a 20 s; `review-q8` usa `parakeet-cli scene` sul WAV completo. Entrambi includono segmentazione/diarizzazione e un caricamento per clip. Questi tempi sono distinti dall’ASR puro residente. La prova da file non misura acquisizione audio, UI, assegnazione dei nomi o riepiloghi LLM.

La prima prova nativa falliva su entrambi i clip dopo il messaggio di trascrizione completata: exit 0xc0000005, modulo `VCOMP140.DLL_unloaded` nei due eventi Windows. La DLL OpenMP veniva scaricata mentre i suoi worker erano ancora attivi. La correzione conserva soltanto questo runtime fino alla chiusura del processo, liberando sessione e modelli. Il test A conserva binario/report/log precedenti; il test B usa il binario ricompilato e termina correttamente su entrambi i clip. La fixture Windows crea worker OpenMP e verifica che il runtime resti caricato dopo la chiusura della sessione. API: [GetModuleHandleExA con FLAG_PIN](https://learn.microsoft.com/en-us/windows/win32/api/libloaderapi/nf-libloaderapi-getmodulehandleexa).

## Ripetibilità e file

I punteggi sono stati ricalcolati dalle trascrizioni salvate e confrontati con i conteggi degli errori nei report. Audio, riferimenti, pesi, scorer e libreria sono verificati con SHA256; le versioni del runner registrate nei report sono archiviate. I risultati iniziali Q8/F16 sono conservati, ma le tabelle usano le ripetizioni fatte dopo la preparazione degli ambienti per evitare la sovrapposizione con gli smoke test GPU. Parakeet CUDA aveva elaborato tutti i casi ma falliva alla chiusura del processo con «CUDA error: driver shutting down»; il runner ora libera esplicitamente la DLL dopo il contesto ASR, prima della chiusura del driver. Le tabelle usano due passaggi completi ripetuti con exit 0 dopo questa correzione. Il JSON riporta le verifiche di ripetizione e gli intervalli bootstrap esplorativi sul subset FLEURS IT.

Download nuovi, stima conservativa comprensiva di modelli/configurazioni/runtime, wheel e corpus: **26.629 GB**, entro i 50 GB autorizzati. Il conteggio del corpus comprende anche copie estratte localmente; i modelli Q8/base già presenti non sono nuovi download. Gli ambienti installati e i file decompressi occupano ulteriore spazio su disco.

Risultati aggregati, senza testi personali: [JSON](MEETING_ASR_RESULTS_20261009.json). Corpus, riferimenti, trascrizioni, log, CSV per caso e report completi restano nel worktree sotto `build/asr-benchmark-20261009/`, escluso da Git. Modelli nuovi, wheel e sorgenti pubbliche sono sotto `D:/models/fbdb-asr-benchmark-20261009/`. Nessun ASR cloud o riepilogo LLM è stato usato.

Per ripetere un modello dal worktree, con gli asset locali già preparati e una cartella di output nuova:

```powershell
.\build\asr-benchmark-20261009\venv\Scripts\python.exe .\scripts\meeting_benchmark.py --config .\build\asr-benchmark-20261009\config.json --variant parakeet-q8 --corpus .\build\asr-benchmark-20261009\corpus\cases.json --output .\build\asr-benchmark-20261009\manual-runs\q8-02
```

Canary richiede `canary-venv`; i profili INT8 usano `daily-config.json`. Un riferimento corretto o aggiunto crea un nuovo insieme di valutazione: non va trattato come regressione del modello sul vecchio input.

## Fonti e revisioni

- Motore: [parakeet.cpp v0.6.1](https://github.com/mudler/parakeet.cpp/releases/tag/v0.6.1), [faster-whisper](https://github.com/SYSTRAN/faster-whisper), [Qwen3-ASR](https://github.com/QwenLM/Qwen3-ASR), [Canary Transformers](https://huggingface.co/docs/transformers/main/model_doc/canary).
- Corpus pubblici, CC BY 4.0: [FLEURS](https://huggingface.co/datasets/google/fleurs), [LibriSpeech](https://huggingface.co/datasets/openslr/librispeech_asr), [AMI](https://huggingface.co/datasets/edinburghcstr/ami). Revisioni e ID di selezione nei manifest locali.
- GGUF Parakeet/Nemotron/Redux: [mudler/parakeet-cpp-gguf](https://huggingface.co/mudler/parakeet-cpp-gguf), revisione `741158ae71e64ef5c89385862c18f777d07a97a1`; il Q8 preesistente è identificato dal proprio SHA256 nei report.
- Dizionario ortografico: [Whisper large-v3 normalizer](https://huggingface.co/openai/whisper-large-v3/blob/06f233fe06e710322aca913c1bc4249a0d71fce1/normalizer.json).

| Checkpoint scaricato | Revisione |
| --- | --- |
| [Systran/faster-whisper-small](https://huggingface.co/Systran/faster-whisper-small) | `536b0662742c02347bc0e980a01041f333bce120` |
| [Systran/faster-whisper-medium](https://huggingface.co/Systran/faster-whisper-medium) | `08e178d48790749d25932bbc082711ddcfdfbc4f` |
| [Systran/faster-whisper-large-v3](https://huggingface.co/Systran/faster-whisper-large-v3) | `edaa852ec7e145841d8ffdb056a99866b5f0a478` |
| [mobiuslabsgmbh/faster-whisper-large-v3-turbo](https://huggingface.co/mobiuslabsgmbh/faster-whisper-large-v3-turbo) | `0a363e9161cbc7ed1431c9597a8ceaf0c4f78fcf` |
| [Qwen/Qwen3-ASR-0.6B](https://huggingface.co/Qwen/Qwen3-ASR-0.6B) | `5eb144179a02acc5e5ba31e748d22b0cf3e303b0` |
| [Qwen/Qwen3-ASR-1.7B](https://huggingface.co/Qwen/Qwen3-ASR-1.7B) | `7278e1e70fe206f11671096ffdd38061171dd6e5` |
| [nvidia/canary-1b-v2](https://huggingface.co/nvidia/canary-1b-v2) | `d455706339a6b32e1aa40f82c713a482a0c938e2` |

Whisper base riutilizza la snapshot Systran `ebe41f70d5b6dfa9166e2c581c45c9c0cfc57b66`, già presente prima della prova. Il confronto non cambia automaticamente il modello live di Franco e non attiva una revisione al termine del meeting.
