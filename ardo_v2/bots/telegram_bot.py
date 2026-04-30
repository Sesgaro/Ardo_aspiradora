import telebot
import os
import subprocess
from faster_whisper import WhisperModel
from core.llm import clean_text_for_tts

print("Oido neuronal: Cargando Whisper en CPU (Ahorro de VRAM)...")
stt_model = WhisperModel("medium", device="cuda", compute_type="int8")

def iniciar_bot(token: str, cerebro, ardo_tts):
    bot = telebot.TeleBot(token)

    @bot.message_handler(content_types=['text'])
    def manejar_texto(message):
        chat_id = str(message.chat.id)
        print(f"Telegram Texto [{message.from_user.first_name}]: {message.text}")
        bot.send_chat_action(chat_id, 'typing')
        respuesta = cerebro.chat(message.text, chat_id=chat_id)
        bot.reply_to(message, respuesta)

    @bot.message_handler(content_types=['voice'])
    def manejar_audio(message):
        chat_id = str(message.chat.id)
        print(f"Telegram Voice [{message.from_user.first_name}] recibido.")
        try:
            # 1. Descarga y transcripcion
            file_info = bot.get_file(message.voice.file_id)
            descargado_bytes = bot.download_file(file_info.file_path)
            
            archivo_temp_in = f"temp_in_{message.voice.file_id}.ogg"
            with open(archivo_temp_in, "wb") as f:
                f.write(descargado_bytes)
                
            segments, _ = stt_model.transcribe(archivo_temp_in, language="es")
            texto_usuario = "".join([segment.text for segment in segments]).strip()
            os.remove(archivo_temp_in)

            if not texto_usuario:
                bot.reply_to(message, "No se detecto audio.")
                return

            print(f"Escucho: '{texto_usuario}'")
            
            # 2. Feedback inicial al usuario
            t_low = texto_usuario.lower()
            if any(w in t_low for w in ["clima", "tiempo", "temperatura"]):
                bot.send_message(chat_id, "Consultando sistemas meteorologicos...")
            elif any(w in t_low for w in ["busca", "internet", "quien", "que es"]):
                bot.send_message(chat_id, "Iniciando busqueda en la red...")

            # 3. Procesamiento LLM
            bot.send_chat_action(chat_id, 'record_voice')
            respuesta_texto = cerebro.chat(texto_usuario, chat_id=chat_id)
            
            # 4. Limpieza y Generacion de voz
            texto_para_voz = clean_text_for_tts(respuesta_texto)
            print(f"ARDO (Texto limpio para voz): {texto_para_voz}")

            ruta_wav = f"res_{message.voice.file_id}.wav"
            ruta_ogg = f"res_{message.voice.file_id}.ogg"
            
            ardo_tts.generate_to_file(texto_para_voz, ruta_wav)
            
            # 5. Conversion a Opus para Telegram
            comando = [
                "ffmpeg", "-y", "-i", ruta_wav,
                "-c:a", "libopus", "-b:a", "32k", "-ar", "48000",
                ruta_ogg
            ]
            subprocess.run(comando, capture_output=True)

            # 6. Envio de audio con timeout extendido
            with open(ruta_ogg, "rb") as audio_file:
                bot.send_voice(
                    chat_id, 
                    audio_file, 
                    reply_to_message_id=message.message_id,
                    timeout=60
                )
            
            if os.path.exists(ruta_wav): os.remove(ruta_wav)
            if os.path.exists(ruta_ogg): os.remove(ruta_ogg)

        except Exception as e:
            print(f"Error en manejar_audio: {e}")
            bot.send_message(chat_id, "Fallo en el sistema de audio o conexion.")

    print("Bot de Telegram en linea y a la espera.")
    bot.infinity_polling(timeout=60, long_polling_timeout=60)
