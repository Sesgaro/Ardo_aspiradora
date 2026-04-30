#include <MKL25Z4.H>

void delay_us(int n);
float medir_distancia(void);

int main(void) {
    float distancia;



    // Relojes
    SIM->SCGC5 |= SIM_SCGC5_PORTA_MASK | SIM_SCGC5_PORTD_MASK; // Reloj Porta A y D
    SIM->SCGC6 |= SIM_SCGC6_TPM0_MASK;  // Reloj TPM0
    SIM->SOPT2 |= SIM_SOPT2_TPMSRC(1);

    // Pines
    PORTA->PCR[12] = 0x100;    // PTA12 como GPIO (Trig)
    PTA->PDDR |= (1 << 12);    // Salida
    PORTD->PCR[4] = 0x100;     // PTD4 como GPIO (Echo)
    PTD->PDDR &= ~(1 << 4);    // Entrada

    // Timer TPM0 para microsegundos
    TPM0->SC = 0;              // Desactivar para configurar
    TPM0->MOD = 0xFFFF;        // Valor máximo
    TPM0->SC = 0x08 | 0x05;    // Prescaler 1:32

    while(1) {
        distancia = medir_distancia();

        for(int i=0; i<500000; i++); // Retraso simple entre lecturas
    }
}

float medir_distancia(void) {
    int eco_tiempo = 0;

    // Enviar pulso de disparo (Trigger) de 10us
    PTA->PSOR = (1 << 12);     // Trig = HIGH
    delay_us(10);
    PTA->PCOR = (1 << 12);     // Trig = LOW

    // Esperar a que el Echo suba
    while(!(PTD->PDIR & (1 << 4)));

    TPM0->CNT = 0;             // Resetear contador del timer

    // Esperar a que el Echo baje
    while(PTD->PDIR & (1 << 4));

    eco_tiempo = TPM0->CNT;    // Leer cuánto tiempo pasó

    // Conversión a cm: (Tiempo * Velocidad Sonido) / 2
    // Con prescaler 32 a 20.97MHz: Distancia = ticks * 0.02617
    //20.97MHz / 32 = 655,312.5
    // 1/655,312.5 = 1.5259e-6
    // 1.5259e-6 * 34300 / 2 = 0.02617

    return (float)eco_tiempo * 0.02617f;
}

void delay_us(int n) {
    TPM0->CNT = 0;

    while(TPM0->CNT < (n * 1.5));
}
