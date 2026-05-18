import io
import wave
import serial
import threading
import uvicorn
from fastapi import FastAPI, UploadFile, File, HTTPException, Form
from faster_whisper import WhisperModel

app = FastAPI(title="Ardo STT Unified Server")

# ── CONFIGURACIÓN DE IA (GPU NVIDIA) ──
MODEL_SIZE = "base" # Cambia a "medium" si quieres más precisión
DEVICE = "cuda"
COMPUTE_TYPE = "float16"

# ── CONFIGURACIÓN DEL USB DE ARDO ──
SERIAL_PORT = "/dev/ttyACM2" 
BAUD_RATE = 115200

print(f"[*] Cargando modelo Whisper '{MODEL_SIZE}' en modo {DEVICE}...")
model = WhisperModel(MODEL_SIZE, device=DEVICE, compute_type=COMPUTE_TYPE)
print("[+] Modelo cargado en la VRAM correctamente y listo.")

# ════════════════════════════════════════════════════════════════════════
# 1. HILO EN SEGUNDO PLANO PARA LEER EL USB DEL ESP32
# ════════════════════════════════════════════════════════════════════════
def create_wav_in_memory(raw_pcm_bytes):
    """Convierte los bytes del USB a WAV en la memoria RAM"""
    wav_io = io.BytesIO()
    with wave.open(wav_io, 'wb') as wav_file:
        wav_file.setnchannels(1)       # Mono
        wav_file.setsampwidth(2)       # 16 bits
        wav_file.setframerate(16000)   # 16 kHz
        wav_file.writeframes(raw_pcm_bytes)
    wav_io.seek(0)
    return wav_io

def usb_listener_task():
    try:
        ser = serial.Serial(SERIAL_PORT, BAUD_RATE)
        print(f"[*] Hilo USB iniciado: Escuchando a Ardo en {SERIAL_PORT}...")
    except Exception as e:
        print(f"[-] Error abriendo USB. Revisa que Ardo esté conectado en {SERIAL_PORT}")
        return

    audio_buffer = bytearray()
    in_frame = False

    while True:
        try:
            # Buscar la firma mágica: 0xAA 0xBB (La que programamos en ESPHome)
            if ser.read(1) == b'\xaa' and ser.read(1) == b'\xbb':
                cmd = ser.read(1)[0]
                len_hi = ser.read(1)[0]
                len_lo = ser.read(1)[0]
                payload_len = (len_hi << 8) | len_lo
                payload = ser.read(payload_len) if payload_len > 0 else b''

                if cmd == 0x01:  # CMD_AUDIO (Recibiendo voz)
                    if not in_frame:
                        print("\n[*] Ardo (USB) está escuchando...")
                        in_frame = True
                    audio_buffer.extend(payload)

                elif cmd == 0x02:  # CMD_STREAM_END (Fin de frase)
                    if in_frame and len(audio_buffer) > 0:
                        print(f"[*] Ardo (USB) terminó. Procesando {len(audio_buffer)} bytes directo en GPU...")
                        
                        wav_file = create_wav_in_memory(audio_buffer)
                        
                        # Inferencia local directa (bypasseando la red)
                        segments, info = model.transcribe(
                            wav_file,
                            task="transcribe",
                            language="es",
                            beam_size=5,
                            vad_filter=True,
                            vad_parameters=dict(min_silence_duration_ms=500)
                        )
                        
                        full_text = "".join([s.text for s in segments]).strip()
                        print(f" \033[92m-> Ardo dijo:\033[0m {full_text}")
                        
                        # AQUÍ PUEDES AGREGAR EL CÓDIGO PARA MANDAR "full_text" A OLLAMA
                        
                        audio_buffer.clear()
                        in_frame = False
                    else:
                        audio_buffer.clear()
                        in_frame = False
                        
        except Exception as e:
            print(f"Error inesperado en lectura USB: {e}")

# ════════════════════════════════════════════════════════════════════════
# 2. ENDPOINT FASTAPI PARA LA OTRA PC POR RED
# ════════════════════════════════════════════════════════════════════════
@app.post("/v1/audio/transcriptions")
async def transcribe_audio(
    file: UploadFile = File(...),
    task: str = Form("transcribe"),  
    language: str = Form("es")       
):
    if not file.filename:
        raise HTTPException(status_code=400, detail="Archivo de audio no válido.")

    try:
        audio_bytes = await file.read()
        audio_file = io.BytesIO(audio_bytes)

        # Inferencia a través de la red
        segments, info = model.transcribe(
            audio_file,
            task=task,
            language=language if language else None,
            beam_size=5,
            vad_filter=True,
            vad_parameters=dict(min_silence_duration_ms=500)
        )

        full_text = "".join([segment.text for segment in segments]).strip()

        return {
            "text": full_text if full_text else "",
            "language": info.language,
            "language_probability": round(info.language_probability, 4)
        }

    except Exception as e:
        raise HTTPException(status_code=500, detail=f"Error en el servidor de IA: {str(e)}")

# ════════════════════════════════════════════════════════════════════════
# 3. ARRANQUE DEL SISTEMA
# ════════════════════════════════════════════════════════════════════════
if __name__ == "__main__":
    # 1. Levantar el hilo del USB en modo Daemon (para que se cierre al apagar el script)
    usb_thread = threading.Thread(target=usb_listener_task, daemon=True)
    usb_thread.start()

    # 2. Levantar el servidor Web para la otra PC en el puerto 6767
    uvicorn.run(app, host="0.0.0.0", port=6767)
