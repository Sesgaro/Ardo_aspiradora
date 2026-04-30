import soundfile as sf
import numpy as np
import os
from kokoro import KPipeline

os.environ["HF_HUB_OFFLINE"] = "1"

class ArdoTTS:
    def __init__(self, voice="em_alex"):
        print("Sintetizador: Cargando Kokoro TTS en el procesador...")
        self.voice = voice
        self.pipeline = KPipeline(lang_code='e', device='cpu')
        print("Sintetizador: Boca Kokoro en linea.")

    def generate_to_file(self, text: str, output_path: str):
        text_clean = text.replace('\n', ' ').strip()
        
        generator = self.pipeline(text_clean, voice=self.voice, speed=1.0)
        
        audio_chunks = []
        for graphemes, phonemes, audio in generator:
            audio_chunks.append(audio)
            
        if audio_chunks:
            audio_completo = np.concatenate(audio_chunks)
            sf.write(output_path, audio_completo, 24000)
        else:
            print("Sistemas: Error, Kokoro no pudo generar el audio.")
