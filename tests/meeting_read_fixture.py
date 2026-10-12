import argparse
from pathlib import Path
import time

parser = argparse.ArgumentParser()
parser.add_argument('--input', type=Path, required=True)
parser.add_argument('--model', required=True)
options = parser.parse_args()
text = options.input.read_text(encoding='utf-8-sig')
assert text.strip()
observed = Path(str(options.input) + '.observed')
temporary = Path(str(observed) + '.tmp')
temporary.write_text(text, encoding='utf-8', newline='')
temporary.replace(observed)
print('fixture reading ready', flush=True)
if options.model == 'fail':
    raise SystemExit(1)
time.sleep(2 if options.model == 'slow' else 0.1)
