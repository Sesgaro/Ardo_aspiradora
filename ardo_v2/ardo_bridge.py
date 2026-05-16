#!/usr/bin/env python3
"""
Ardo USB Bridge — comunicación bidireccional con ESP32-S3
Protocolo: [0xAA][0xBB][CMD][LEN_H][LEN_L][PAYLOAD]
Los logs de ESPHome son texto plano y se ignoran automáticamente.
"""
import serial
import struct
import sys
import time
import threading
import numpy as np
from pathlib import Path

# =============================================
# CONFIGURACIÓN
# =============================================
USB_PORT        = "/dev/ttyACM7"
USB_BAUD        = 921600
ARCHIVO_AUDIO   = "ardo_audio.raw"

# Protocolo (espejo del .h)
FRAME_MAGIC_0   = 0xAA
FRAME_MAGIC_1   = 0xBB

CMD_AUDIO         = 0x01
CMD_HEARTBEAT     = 0x02
CMD_WAKE_DETECTED = 0x03
CMD_STREAM_END    = 0x04

CMD_STOP          = 0x80
CMD_TTS_START     = 0x40
CMD_TTS_END       = 0x20
CMD_LED_R         = 0x10
CMD_LED_G         = 0x08
CMD_LED_B         = 0x04
CMD_LED_OFF       = 0x02

SILENCE_THRESHOLD = 300
SILENCE_GRACE_S   = 2.0
SILENCE_DURATION  = 1.5
AUDIO_SAMPLE_RATE = 16000
TTS_SAMPLE_RATE   = 24000
TTS_CHUNK_SIZE    = 1100

# =============================================
# FRAMING
# =============================================
def build_frame(cmd: int, payload: bytes = b"") -> bytes:
    length = len(payload)
    header = bytes([FRAME_MAGIC_0, FRAME_MAGIC_1, cmd,
                    (length >> 8) & 0xFF, length & 0xFF])
    return header + payload

# =============================================
# DETECTOR DE SILENCIO
# =============================================
class SilenceDetector:
    def __init__(self, threshold=SILENCE_THRESHOLD,
                 grace=SILENCE_GRACE_S, duration=SILENCE_DURATION):
        self.threshold   = threshold
        self.grace       = grace
        self.duration    = duration
        self.active      = False
        self.triggered   = False
        self.last_sound  = 0.0
        self.grace_until = 0.0
        self.on_silence  = None

    def start(self):
        now = time.time()
        self.last_sound  = now
        self.triggered   = False
        self.active      = True
        self.grace_until = now + self.grace

    def stop(self):
        self.active    = False
        self.triggered = False

    def feed(self, pcm_bytes: bytes):
        if not self.active or self.triggered:
            return
        if time.time() < self.grace_until:
            return
        if len(pcm_bytes) < 2:
            return
        samples = np.frombuffer(pcm_bytes, dtype=np.int16).astype(np.float32)
        rms = float(np.sqrt(np.mean(samples ** 2)))
        if rms > self.threshold:
            self.last_sound = time.time()
        elif (time.time() - self.last_sound) > self.duration:
            self.triggered = True
            self.active    = False
            if self.on_silence:
                self.on_silence()

# =============================================
# BRIDGE PRINCIPAL
# =============================================
class ArdoBridge:
    def __init__(self, port=USB_PORT, baud=USB_BAUD):
        self.port     = port
        self.baud     = baud
        self.ser      = None
        self.running  = False
        self._lock    = threading.Lock()

        self.total_bytes  = 0
        self.frames_audio = 0
        self.heartbeats   = 0
        self.streaming    = False

        self.silence = SilenceDetector()
        self.silence.on_silence = self._on_silence

        # Callbacks externos — conecta aquí tu pipeline de IA
        self.on_wake_detected = None
        self.on_stream_end    = None
        self.on_audio_chunk   = None

    # ------------------------------------------
    # CONEXIÓN
    # ------------------------------------------
    def connect(self) -> bool:
        print(f"🔌 Conectando a {self.port} @ {self.baud} baud...")
        try:
            self.ser = serial.Serial(
                port=self.port,
                baudrate=self.baud,
                timeout=0.1,
                write_timeout=5.0
            )
            print(f"✅ Conectado a {self.port}")
            return True
        except serial.SerialException as e:
            print(f"❌ Error: {e}")
            print(f"   Verifica permisos: sudo usermod -aG dialout $USER")
            return False

    def disconnect(self):
        self.running = False
        if self.ser and self.ser.is_open:
            self.ser.close()

    # ------------------------------------------
    # ESCRITURA THREAD-SAFE
    # ------------------------------------------
    def _write(self, data: bytes):
        with self._lock:
            try:
                self.ser.write(data)
                self.ser.flush()
            except serial.SerialException as e:
                print(f"\n❌ Error de escritura: {e}")

    # ------------------------------------------
    # COMANDOS
    # ------------------------------------------
    def send_stop(self):
        self._write(build_frame(CMD_STOP))
        print("\n🛑 CMD_STOP enviado")

    def send_led(self, color: str):
        mapping = {'r': CMD_LED_R, 'g': CMD_LED_G,
                   'b': CMD_LED_B, 'off': CMD_LED_OFF}
        self._write(build_frame(mapping.get(color.lower(), CMD_LED_OFF)))

    def send_tts(self, audio_data: np.ndarray, sample_rate: int = TTS_SAMPLE_RATE):
        """
        Envía audio TTS al ESP32.
        audio_data: numpy array float32 [-1,1] o int16
        """
        if audio_data.dtype != np.int16:
            audio_int16 = np.clip(audio_data * 32767, -32768, 32767).astype(np.int16)
        else:
            audio_int16 = audio_data

        raw      = audio_int16.tobytes()
        total_kb = len(raw) / 1024
        dur_s    = len(raw) / (sample_rate * 2)
        print(f"\n🔊 Enviando TTS: {total_kb:.1f} KB ({dur_s:.1f}s)...")

        self._write(build_frame(CMD_TTS_START))
        time.sleep(0.02)

        sent = 0
        for i in range(0, len(raw), TTS_CHUNK_SIZE):
            chunk = raw[i:i + TTS_CHUNK_SIZE]
            self._write(build_frame(0x00, chunk))
            sent += len(chunk)
            print(f"  Enviando... {sent/len(raw)*100:.0f}%", end='\r')
            time.sleep(0.001)

        time.sleep(0.02)
        self._write(build_frame(CMD_TTS_END))
        print(f"\n✅ TTS enviado ({total_kb:.1f} KB)")

    # ------------------------------------------
    # CALLBACKS INTERNOS
    # ------------------------------------------
    def _on_silence(self):
        print("\n🔇 Silencio detectado → Cerrando micrófono e iniciando IA...")
        self.streaming = False
        self.silence.stop()
        
        # 1. Le decimos a la ESP32 que deje de mandar audio y apague el LED
        self.send_stop()
        
        # 2. ¡LA MAGIA FALTANTE! 
        # Disparamos el evento para que main.py arranque a Whisper y Qwen
        if self.on_stream_end:
            self.on_stream_end()

    # ------------------------------------------
    # PARSER DE FRAMES
    # ------------------------------------------
    def _dispatch(self, cmd: int, payload: bytes, audio_file):
        if cmd == CMD_HEARTBEAT:
            self.heartbeats += 1
            print(f"\n💓 Heartbeat #{self.heartbeats}")

        elif cmd == CMD_WAKE_DETECTED:
            print(f"\n🎤 Wake word — grabando...")
            self.streaming   = True
            self.total_bytes = 0
            self.frames_audio = 0
            # Limpiar archivo de audio
            audio_file.seek(0)
            audio_file.truncate(0)
            self.silence.start()
            if self.on_wake_detected:
                self.on_wake_detected()

        elif cmd == CMD_AUDIO:
            audio_file.write(payload)
            audio_file.flush()
            self.total_bytes  += len(payload)
            self.frames_audio += 1
            self.silence.feed(payload)
            if self.on_audio_chunk:
                self.on_audio_chunk(payload)
            dur = self.total_bytes / (AUDIO_SAMPLE_RATE * 2)
            print(f"🎙️  #{self.frames_audio} — {len(payload)}B | "
                  f"{self.total_bytes/1024:.1f}KB ({dur:.1f}s)", end='\r')

        elif cmd == CMD_STREAM_END:
            print(f"\n✅ Stream fin — {self.total_bytes/1024:.1f} KB")
            self.streaming = False
            self.silence.stop()


        else:
            print(f"\n⚠️  CMD desconocido: 0x{cmd:02X} ({len(payload)}B)")

    # ------------------------------------------
    # LOOP PRINCIPAL DE RECEPCIÓN
    # ------------------------------------------
    def run(self, audio_output_path: str = ARCHIVO_AUDIO):
        """Bucle bloqueante. Llama en un hilo si necesitas correr otras cosas."""
        self.running = True

        # Estados del parser
        (WAIT_MAGIC0, WAIT_MAGIC1, WAIT_CMD,
         WAIT_LEN_H, WAIT_LEN_L, WAIT_PAYLOAD) = range(6)

        state    = WAIT_MAGIC0
        rx_cmd   = 0
        rx_len   = 0
        rx_buf   = bytearray()

        print(f"\n🎧 Escuchando en {self.port}... (Ctrl+C para parar)\n")

        with open(audio_output_path, 'w+b') as af:
            while self.running:
                try:
                    raw = self.ser.read(1)
                except serial.SerialException as e:
                    print(f"\n❌ Error de lectura: {e}")
                    break

                if not raw:
                    continue

                byte = raw[0]

                if state == WAIT_MAGIC0:
                    # Los logs de ESPHome llegan aquí y se descartan
                    # porque nunca tienen 0xAA seguido de 0xBB
                    if byte == FRAME_MAGIC_0:
                        state = WAIT_MAGIC1

                elif state == WAIT_MAGIC1:
                    state = WAIT_CMD if byte == FRAME_MAGIC_1 else WAIT_MAGIC0

                elif state == WAIT_CMD:
                    rx_cmd = byte
                    state  = WAIT_LEN_H

                elif state == WAIT_LEN_H:
                    rx_len = byte << 8
                    state  = WAIT_LEN_L

                elif state == WAIT_LEN_L:
                    rx_len |= byte
                    if rx_len == 0:
                        self._dispatch(rx_cmd, b"", af)
                        state = WAIT_MAGIC0
                    elif rx_len > 4096:
                        print(f"\n⚠️  Frame sospechoso len={rx_len}")
                        state = WAIT_MAGIC0
                    else:
                        rx_buf = bytearray()
                        state  = WAIT_PAYLOAD

                elif state == WAIT_PAYLOAD:
                    rx_buf.append(byte)
                    if len(rx_buf) >= rx_len:
                        self._dispatch(rx_cmd, bytes(rx_buf), af)
                        state = WAIT_MAGIC0

    def print_summary(self):
        print(f"\n{'='*50}")
        print(f"⏹️  Sesión terminada")
        print(f"   Heartbeats:   {self.heartbeats}")
        print(f"   Frames audio: {self.frames_audio}")
        print(f"   Audio total:  {self.total_bytes/1024:.1f} KB")
        if self.total_bytes > 0:
            dur = self.total_bytes / (AUDIO_SAMPLE_RATE * 2)
            print(f"   Duración:     {dur:.1f}s")
            print(f"\n▶️  ffplay -f s16le -ar {AUDIO_SAMPLE_RATE} -ac 1 {ARCHIVO_AUDIO}")


# =============================================
# USO STANDALONE
# =============================================
def main():
    print("🚀 Ardo USB Bridge")
    print("=" * 50)

    bridge = ArdoBridge(port=USB_PORT, baud=USB_BAUD)
    if not bridge.connect():
        sys.exit(1)

    try:
        bridge.run()
    except KeyboardInterrupt:
        pass
    finally:
        bridge.disconnect()
        bridge.print_summary()


# =============================================
# USO COMO LIBRERÍA:
#
#   from ardo_bridge import ArdoBridge
#   import soundfile as sf, numpy as np, threading
#
#   bridge = ArdoBridge()
#   bridge.connect()
#
#   def pipeline():
#       audio, sr = sf.read("ardo_audio.raw",
#                           samplerate=16000, channels=1,
#                           format='RAW', subtype='PCM_16',
#                           endian='LITTLE')
#       # ... STT → LLM → TTS ...
#       tts_audio, tts_sr = generar_tts(respuesta)
#       bridge.send_tts(tts_audio, tts_sr)
#
#   bridge.on_stream_end = pipeline
#   threading.Thread(target=bridge.run, daemon=True).start()
# =============================================

if __name__ == "__main__":
    main()
