import torch

# --- PARCHE DE SEGURIDAD PARA PYTORCH 2.6 ---
# Apagamos el bloqueo 'weights_only' para que XTTSv2 pueda leer su configuración
_original_load = torch.load
def patched_load(*args, **kwargs):
    kwargs['weights_only'] = False
    return _original_load(*args, **kwargs)
torch.load = patched_load
# --------------------------------------------

from TTS.api import TTS
import io
import soundfile as sf

device = "cuda" if torch.cuda.is_available() else "cpu"
tts = TTS("tts_models/multilingual/multi-dataset/xtts_v2").to(device)

def generar_respuesta_viva(texto, audio_referencia):
    wav = tts.tts(text=texto, speaker_wav=audio_referencia, language="es")
    
    buffer = io.BytesIO()
    sf.write(buffer, wav, 24000, format='OGG')
    buffer.seek(0)
    return buffer.read()

print("Generando voz para Ardo...")
audio_bytes = generar_respuesta_viva("yo si me quede con las ganas de dedearte", "AldoVoice.ogg")

with open("prueba_salida.wav", "wb") as f:
    f.write(audio_bytes)
    
print("¡Audio generado con éxito! Revisa el archivo prueba_salida.wav")
