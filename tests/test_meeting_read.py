import contextlib
import io
from pathlib import Path
import sys
from types import SimpleNamespace
import unittest
import wave

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
import meeting_read


class FixtureVoice:
    def __init__(self):
        self.text = None

    def synthesize(self, text):
        self.text = text
        for size in [80, 160]:
            yield SimpleNamespace(sample_channels=1, sample_width=2, sample_rate=22050,
                                  audio_int16_bytes=b'\0' * size * 2)


class ReadingTests(unittest.TestCase):
    def test_preserves_unicode_text_and_plays_pcm_chunks_in_order(self):
        voice = FixtureVoice()
        played = []
        text = 'Appunti: sì, attività e responsabilità.\nSeconda riga.'
        with contextlib.redirect_stdout(io.StringIO()):
            result = meeting_read.synthesize(voice, text, played.append)
        self.assertEqual(voice.text, text)
        self.assertEqual(result, {'blocks': 2, 'audio_frames': 240})
        for audio, expected in zip(played, [80, 160]):
            with wave.open(io.BytesIO(audio)) as wav:
                self.assertEqual((wav.getnchannels(), wav.getsampwidth(), wav.getframerate()), (1, 2, 22050))
                self.assertEqual(wav.getnframes(), expected)

    def test_render_only_synthesizes_without_playback(self):
        with contextlib.redirect_stdout(io.StringIO()):
            result = meeting_read.synthesize(FixtureVoice(), 'Test locale.', None)
        self.assertEqual(result['audio_frames'], 240)

    def test_empty_text_never_calls_voice_or_player(self):
        voice = FixtureVoice()
        played = []
        for text in ['', ' \n\t']:
            with self.assertRaisesRegex(ValueError, 'no text'):
                meeting_read.synthesize(voice, text, played.append)
        self.assertIsNone(voice.text)
        self.assertEqual(played, [])


if __name__ == '__main__':
    unittest.main()
