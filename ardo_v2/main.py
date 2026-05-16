import time
import threading
import os
import soundfile as sf
import numpy as np
from faster_whisper import WhisperModel

from core.memory import ArdoMemory
from core.llm import ArdoBrain, clean_text_for_tts
from core.tts import ArdoTTS
from bots.telegram_bot import iniciar_bot
from ardo_bridge import ArdoBridge

TELEGRAM_TOKEN = "8747383095:AAEt0ZlcJfiwdUVqKbUScz1SjHakIdPRMsI" 

def hilo_telegram(token, cerebro, ardo_tts):
    """Mantiene el bot vivo incluso con micro-cortes de internet"""
    while True:
        try:
            iniciar_bot(token, cerebro, ardo_tts)
        except Exception as e:
            print(f"⚠️ Conexión perdida. Reintentando en 5 segundos... Error: {e}")
            time.sleep(5)

def main():
    print("=== Iniciando Sistemas de ARDO ===")
    
    # 1. Base de datos
    print("Cargando ChromaDB (Memoria a largo plazo)...")
    memoria = ArdoMemory()
    
    # 2. La Boca (Modelo pesado a la VRAM)
    ardo_tts = ArdoTTS(voice="em_alex") 
    
    # 3. El Cerebro LLM
    print("Conectando con Qwen...")
    cerebro = ArdoBrain(memoria)

    # 4. El Oído (Cargamos Whisper aquí para que el USB lo pueda usar)
    print("Cargando Whisper STT para el hardware local...")
    # Si tienes GPU en tu CachyOS, cambia "cpu" por "cuda"
    stt_model = WhisperModel("medium", device="cpu", compute_type="int8")
    
    # ---------------------------------------------------------
    # 5. INTEGRACIÓN DEL PUENTE USB (HARDWARE)
    # ---------------------------------------------------------
    print("Iniciando Bridge USB...")
    # Asegúrate de que el puerto coincida con tu sistema (ttyACM0, ttyACM1, etc.)
    bridge = ArdoBridge(port="/dev/ttyACM10", baud=921600)

    def pipeline_ia():
        """Se ejecuta cuando el ESP32 termina de enviar el audio (on_stream_end)"""
        print("\n🧠 ARDO: Procesando audio del entorno físico...")
        bridge.send_led('g') # LED Verde = Pensando
        
        try:
            if not os.path.exists("ardo_audio.raw") or os.path.getsize("ardo_audio.raw") < 1000:
                return # Ignorar si el archivo está vacío o es muy pequeño
            # A) STT: Bypass de FFmpeg leyendo el RAW directamente a Numpy
            # Leemos el PCM de 16-bits y lo normalizamos a float32 (rango -1.0 a 1.0)
            audio_array = np.fromfile("ardo_audio.raw", dtype=np.int16).astype(np.float32) / 32768.0
            with open("ardo_audio.raw", "wb") as f: f.truncate(0)
            # Le pasamos el array matemático directo a Whisper, sin usar el disco duro!
            segments, _ = stt_model.transcribe(audio_array, language="es")
            texto_usuario = "".join([segment.text for segment in segments]).strip()

            if not texto_usuario:
                print("ARDO: Silencio o ruido indescifrable detectado. Cancelando.")
                bridge.send_stop()
                return

            print(f"🗣️  Usuario (Local): '{texto_usuario}'")

            # B) LLM: Pensar la respuesta
            respuesta = cerebro.chat(texto_usuario, chat_id="hardware_usb")
            print(f"🤖 ARDO: {respuesta}")

            # C) TTS: Generar la voz
            texto_limpio = clean_text_for_tts(respuesta)
            ruta_temp_wav = "temp_usb_response.wav"
            ardo_tts.generate_to_file(texto_limpio, ruta_temp_wav)

            # D) TX: Enviar el audio de regreso al ESP32
            if os.path.exists(ruta_temp_wav):
                # Leemos el wav generado por Kokoro
                audio_data, sr = sf.read(ruta_temp_wav, dtype='int16')
                
                # Enviamos el numpy array al puente USB
                bridge.send_tts(audio_data, sample_rate=sr)
                
                # Limpieza
                os.remove(ruta_temp_wav)
            else:
                bridge.send_stop()

        except Exception as e:
            print(f"❌ Error crítico en pipeline local: {e}")
            bridge.send_stop()

    def al_terminar_stream():
        # Lanzamos el pipeline en un hilo nuevo para no bloquear el RX task del USB
        threading.Thread(target=pipeline_ia, daemon=True).start()

    # Conectamos el evento del bridge a nuestra función
    bridge.on_stream_end = al_terminar_stream

    # Si logra abrir el puerto COM, lanzamos el bucle de escucha en segundo plano
    if bridge.connect():
        hilo_usb = threading.Thread(target=bridge.run, daemon=True)
        hilo_usb.start()
    else:
        print("⚠️ No se pudo iniciar la conexión USB. (Verifica permisos en CachyOS: sudo usermod -aG uucp $USER)")
    # ---------------------------------------------------------

    # 6. Lanzar Telegram
    if TELEGRAM_TOKEN != "":
        hilo_tg = threading.Thread(target=hilo_telegram, args=(TELEGRAM_TOKEN, cerebro, ardo_tts), daemon=True)
        hilo_tg.start()
    else:
        print("Token de Telegram no configurado.")

    # 7. Terminal de texto pura
    while True:
        try:
            texto = input("Terminal: ")
            
            if texto.lower() in ['salir', 'exit', 'quit']:
                print("ARDO: Apagando sistemas...")
                bridge.disconnect()
                break
                
            if texto.startswith('/recordar '):
                dato = texto.replace('/recordar ', '')
                memoria.save_long_term(str(time.time()), dato)
                print("ARDO: [Dato inyectado en base vectorial]")
                continue
                
            respuesta = cerebro.chat(texto)
            print(f"\nARDO: {respuesta}\n")
            
        except KeyboardInterrupt:
            bridge.disconnect()
            break

if __name__ == "__main__":
    main()
