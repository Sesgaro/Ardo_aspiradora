import time
import threading
from core.memory import ArdoMemory
from core.llm import ArdoBrain
from core.tts import ArdoTTS
from bots.telegram_bot import iniciar_bot

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
    # Pásale el nombre exacto de tu archivo de muestra.
    ardo_tts = ArdoTTS(voice="em_alex") 
    # 3. El Cerebro LLM
    print("Conectando con Qwen...")
    cerebro = ArdoBrain(memoria)
    
    # 4. Lanzar Telegram
    if TELEGRAM_TOKEN != "":
        # Le inyectamos el cerebro y la boca neuronal
        hilo = threading.Thread(target=hilo_telegram, args=(TELEGRAM_TOKEN, cerebro, ardo_tts), daemon=True)
        hilo.start()
    else:
        print("Token de Telegram no configurado. Solo terminal activa.\n")

    # 5. Terminal (Siempre es puro texto, como pediste)
    while True:
        try:
            texto = input("Terminal: ")
            
            if texto.lower() in ['salir', 'exit', 'quit']:
                print("ARDO: Entrando en modo de suspensión total. Apagando terminal y bot.")
                break
                
            if texto.startswith('/recordar '):
                dato = texto.replace('/recordar ', '')
                doc_id = str(time.time()) 
                memoria.save_long_term(doc_id, dato)
                print("ARDO: [Dato encriptado en el hipocampo local]")
                continue
                
            # Chat en la terminal, cero audios.
            respuesta = cerebro.chat(texto)
            print(f"\nARDO: {respuesta}\n")
            
        except KeyboardInterrupt:
            break

if __name__ == "__main__":
    main()
