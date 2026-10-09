# Sample e confronti della trascrizione

Il banco prova confronta trascrizioni grezze sullo stesso audio con un testo di
riferimento verificato ascoltandolo. Misura WER (errori sulle parole), CER (errori
sui caratteri), tempo e picco di RAM del processo ASR. Conserva input, output,
log e hash SHA256 dei modelli e degli strumenti. Non chiama un LLM e non invia
audio o testi a un endpoint.

La configurazione iniziale confronta lo stesso Parakeet TDT 0.6B v3 Q8_0:

- `live-q8`: il percorso nativo di Franco, caricamento unico e blocchi di input
  fino a 20 secondi, riprodotti da un WAV già registrato;
- `review-q8`: `parakeet-cli scene` sul WAV completo, con segmentazione e
  attribuzione degli speaker gestite da Parakeet.

Questo misura l'effetto dei due percorsi prima di scegliere un altro modello.
La revisione automatica dopo F6 verrà integrata dopo il confronto sui sample.
F6 mantiene per ora il flusso esistente di chiusura e riepilogo.

## Preparazione

Dalla cartella del worktree Meeting, con Python 3.10 o successivo e gli asset
già preparati secondo [MEETING.md](MEETING.md):

```powershell
python .\scripts\meeting_eval.py init
```

Il corpus predefinito è `meetings\asr-tests` accanto a
`build\native\Release\fbdb.exe`. Contiene `cases.json` e `models.json`.
Per un'altra cartella usa `--corpus "D:\franco-asr-tests"` in tutti i comandi.
`init --executable <fbdb.exe> --assets <cartella>` configura strumenti in
posizioni diverse; `--corpus` controlla sempre la posizione dei sample.

Inizia con tre registrazioni di circa 30–90 secondi:

1. Una persona con il microfono Realtek delle cuffie, con introduzione, nomi,
   numeri, date e frasi normali.
2. Due persone che si alternano, usando anche l'audio di sistema.
3. Parlato rapido, rumore o qualche sovrapposizione delle voci.

In Meeting crea un nuovo incontro, scegli `/meeting source microphone` oppure
`/meeting source both`, registra con Ctrl+Space e chiudi con F6. Nella cartella
mostrata dall'app trovi `audio.wav`.

Crea accanto al WAV un `reference.txt` UTF-8. Riascolta il sample e scrivi le
parole effettivamente pronunciate, nell'ordine, conservando ripetizioni, nomi
e numeri. Il riferimento va controllato indipendentemente dall'ASR. Le note
e il riepilogo dell'incontro non sono un riferimento per la trascrizione.

```powershell
$sampleMeeting = 'PERCORSO_DELLA_CARTELLA_DEL_MEETING'
python .\scripts\meeting_eval.py add --id realtek-01 --audio "$sampleMeeting\audio.wav" --reference "$sampleMeeting\reference.txt" --tag headset --tag one-speaker
```

`add` copia audio e riferimento nel corpus, mantenendo gli originali. Gli ID
non vengono sovrascritti. Il WAV deve essere PCM16 mono a 16 kHz, come quello
prodotto da Meeting. Per un sample privo di parlato usa un riferimento vuoto
e `--kind silence`: il risultato conterà le eventuali parole aggiunte, senza
assegnare un WER a un riferimento senza parole.

I dati restano nella cartella scelta. `meetings/` e `build/` sono esclusi da Git;
se scegli un altro percorso, tieni il corpus fuori dai file versionati.
Il banco prova non attiva registrazioni o microfoni.

## Confronto e regressioni

```powershell
python .\scripts\meeting_eval.py run
```

Le varianti vengono eseguite una alla volta. Ogni esecuzione crea una nuova
cartella sotto `runs`, con `report.md`, `report.json` e output per ogni coppia
sample/modello. Gli errori conservano log e stato di errore, senza produrre
un punteggio di accuratezza.

Conserva il primo report come baseline. Dopo una modifica a Franco, Parakeet
o al modello riesegui gli stessi sample:

```powershell
python .\scripts\meeting_eval.py run --baseline 'PERCORSO_DEL_REPORT_PRECEDENTE\report.json'
```

Il report confronta numero di errori e tempo per ogni sample e variante. Gli
hash di audio e riferimento devono coincidere: correggere il riferimento crea
un input diverso, non dimostra un miglioramento del modello. I casi rimossi,
le nuove varianti e gli ASR falliti sono dichiarati non confrontabili.

Codici di uscita: `0` per esecuzioni riuscite e, con baseline, stessi casi senza
aumento degli errori sulle parole; `1` per ASR fallito, più errori o dati non
confrontabili; `2` per argomenti, corpus o configurazione non validi. Tempo e
RAM non hanno una soglia automatica: vanno valutati rispetto all'accuratezza.

`models.json` contiene strumenti e varianti. Per provare un GGUF compatibile
con Parakeet aggiungi una variante con un nuovo ID, `mode: "review"` e `asr`
uguale al percorso assoluto del modello. `--models <file.json>` permette
configurazioni diverse. Non vengono scaricati altri modelli. Whisper e Qwen ASR
richiederanno un adattatore dedicato dopo la scelta; il primo confronto usa
esclusivamente Parakeet.cpp.

## Come interpretare le misure

- WER e CER ignorano maiuscole, punteggiatura e spazi ripetuti, normalizzano
  Unicode e apostrofi tipografici, conservano accenti e apostrofi interni. Il
  CER comprende gli spazi. I numeri non sono riscritti: `15` e `quindici`
  possono risultare diversi. Queste regole sono versionate nel report.
- Il tempo include il caricamento del modello, ma esclude preparazione e hash.
  Gli hash sono letti prima dei test: non è una misura con cache disco vuota.
  RTF = tempo / durata audio; meno di 1 significa elaborazione più rapida
  della durata del sample.
- La RAM è il picco residente del singolo processo ASR osservato tramite
  Windows o Linux; se non disponibile viene dichiarato. Non include VRAM o
  altri programmi e non misura la convivenza con l'agent.
- `live-q8` riproduce l'ASR da file: non misura acquisizione, latenza della UI,
  pause/resume o frame persi dal dispositivo. WER e CER misurano il testo;
  speaker, nomi, timestamp e riepiloghi richiedono test separati.

I test automatici usano audio e risposte sintetici per verificare metriche,
baseline, log e percorso nativo. L'accuratezza sull'italiano richiede i sample
e i riferimenti controllati.
