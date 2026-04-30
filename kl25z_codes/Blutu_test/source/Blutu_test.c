#include <MKL25Z4.h>

void UART1_Init(void);
void UART1_Write(char data);
void UART1_WriteString(char* str);
void delay_ms(uint32_t n);

int main(void) {
    UART1_Init();

    // Configurar PTE21 como salida (LED de Diagnóstico/Radar)
    SIM->SCGC5 |= SIM_SCGC5_PORTE_MASK;
    PORTE->PCR[21] = PORT_PCR_MUX(1);
    PTE->PDDR |= (1 << 21);

    while (1) {
        // Enviar comando a la ESP32
        UART1_WriteString("AT+RSSI?\r\n");

        // Parpadeo visual en la KL25Z
        PTE->PTOR = (1 << 21);

        // Delay de aproximadamente 1.5 segundos
        delay_ms(200);
    }
}

void UART1_Init(void) {
    // 1. Habilitar relojes para UART1 y Puerto E
    SIM->SCGC4 |= SIM_SCGC4_UART1_MASK;
    SIM->SCGC5 |= SIM_SCGC5_PORTE_MASK;

    // 2. Configurar pines PTE0 (TX) y PTE1 (RX) como ALT 3 (UART1)
    // Limpiamos antes para evitar conflictos
    PORTE->PCR[0] = 0;
    PORTE->PCR[1] = 0;
    PORTE->PCR[0] = PORT_PCR_MUX(3);
    PORTE->PCR[1] = PORT_PCR_MUX(3);

    // 3. Desactivar UART para configuración
    UART1->C2 &= ~(UART_C2_TE_MASK | UART_C2_RE_MASK);

    // 4. Configurar Baud Rate a 9600
    // Bus Clock = 41.94MHz / 2 = 20,971,520 Hz
    // SBR = 20,971,520 / (16 * 9600) = 136.53 -> Usamos 137 (0x89)
    UART1->BDH = 0x00;
    UART1->BDL = 0xA9;

    // 5. Activar Transmisor y Receptor
    UART1->C1 = 0x00;
    UART1->C2 |= (UART_C2_TE_MASK | UART_C2_RE_MASK);
}

void UART1_Write(char data) {
    // Esperar a que el buffer esté vacío
    while (!(UART1->S1 & UART_S1_TDRE_MASK));
    UART1->D = data;
}

void UART1_WriteString(char* str) {
    while(*str) UART1_Write(*str++);
}

void delay_ms(uint32_t n) {
    // Ajustado para 41.94 MHz
    for(volatile int i = 0; i < n * 3500; i++);
}
