import requests

HA_URL = "http://tu-ip-de-home-assistant:8123/api"
HA_TOKEN = "TU_TOKEN_DE_ACCESO"

# Esta es la base de datos simulada de tu casa. 
# Si el usuario pide algo que no está aquí, ARDO lo rechazará.
LUCES_REGISTRADAS = ["light.sala", "light.cuarto", "light.garage"]

def _verificar_luz(entity_id: str) -> bool:
    """Verifica en Home Assistant si el dispositivo realmente existe antes de actuar."""
    # --- CÓDIGO REAL PARA HOME ASSISTANT (Descomentar en el futuro) ---
    # headers = {"Authorization": f"Bearer {HA_TOKEN}"}
    # response = requests.get(f"{HA_URL}/states/{entity_id}", headers=headers)
    # return response.status_code == 200 # Retorna True si existe, False si da 404
    
    # --- CÓDIGO SIMULADO ---
    return entity_id in LUCES_REGISTRADAS

def turn_on_light(entity_id: str) -> str:
    """Enciende una luz, foco o interruptor inteligente en la casa."""
    if not _verificar_luz(entity_id):
        # Limpiamos el nombre para que ARDO lo diga natural (quita el 'light.')
        area = entity_id.replace("light.", "").replace("switch.", "")
        return f"ERROR_NO_EXISTE. Dile al usuario EXACTAMENTE esta frase: 'Hmmm, no encontre ninguna luz en {area}'"
        
    print(f"💡 ARDO ejecutando hardware: Encendiendo {entity_id}...")
    return f"Operación exitosa. Informa que {entity_id} ya está encendido."

def turn_off_light(entity_id: str) -> str:
    """Apaga una luz, foco o interruptor inteligente en la casa."""
    if not _verificar_luz(entity_id):
        area = entity_id.replace("light.", "").replace("switch.", "")
        return f"ERROR_NO_EXISTE. Dile al usuario EXACTAMENTE esta frase: 'Hmmm, no encontre ninguna luz en {area}'"
        
    print(f"🌑 ARDO ejecutando hardware: Apagando {entity_id}...")
    return f"Operación exitosa. Informa que {entity_id} ya está apagado."
