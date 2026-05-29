#include <MKL25Z4.h>
#include <stdint.h>

#define MPU9250_ADDR 0x68
#define WHO_AM_I_REG 0x75


volatile int bandera_20Hz = 0;
float angulo_yaw = 0.0;
float temporizador_mision = 0.0;


void pit_init(void);
void delay_ms(int n);
void i2c_init(void);
unsigned char i2c_read_register(unsigned char dev_addr, unsigned char reg_addr);
void i2c_write_register(unsigned char dev_addr, unsigned char reg_addr, unsigned char data);
void led_rgb_init(void);
void led_verde(void);
void led_rojo(void);
void led_azul(void);
void imu_init(void);
int16_t leer_aceleracion_x(void);
int16_t leer_giroscopio_z(void);
void motores_init(void);
void motor_izq(int velocidad, int adelante);
void motor_der(int velocidad, int adelante);
void detener_motores(void);

// 1. LA CONSTANTE DE CALIBRACIÓN
/*NO MOVER A MENOS QUE NO RECORRA LA DISTANCIA QUE EL USUARIO LE PONE
 * */
float VELOCIDAD_MS = 0.12;

// 2. LA ESTRUCTURA DEL WAYPOINT
typedef struct {
    float metros;       // Distancia a avanzar en metros
    float angulo_giro;  // Grados: Negativo = Izquierda, Positivo = Derecha, 0 = FIN / No girar
} Waypoint;

// 3. LA NUEVA RUTA DEL USUARIO
#define NUM_WAYPOINTS 4
Waypoint ruta[NUM_WAYPOINTS] = {
    {0.10, -90.0}, // WP 0: Avanza 10cm y gira 90° a la Izquierda
    {0.05,  45.0}, // WP 1: Avanza 5cm y gira 45° a la Derecha
    {0.05, -20.0}, // WP 2: Avanza 5cm y gira 20° a la Izquierda
    {0.10,   0.0}  // WP 3: Avanza 10cm y TERMINA
};



// FUNCIÓN PRINCIPAL

int main(void) {
    led_rgb_init();
    motores_init();

    led_azul();
    delay_ms(500);

    i2c_init();
    delay_ms(100);
    imu_init();
    delay_ms(50);

    pit_init();

    int estado_mision = 0;
    int wp_actual = 0;
    float tiempo_objetivo = 0.0;

    while(1) {
        if (bandera_20Hz == 1) {
            bandera_20Hz = 0;
            temporizador_mision += 0.05;

            // --- CÁLCULO DE ODOMETRÍA (Rotación) ---
            int16_t gyro_z_crudo = leer_giroscopio_z();
            if (gyro_z_crudo > -300 && gyro_z_crudo < 300) {
                gyro_z_crudo = 0;
            }
            float velocidad_angular = (float)gyro_z_crudo / 131.0;
            angulo_yaw = angulo_yaw + (velocidad_angular * 0.05);


            // MÁQUINA DE ESTADOS VECTORIAL

            // ESTADO 0: Leer los metros y convertirlos a segundos
            if (estado_mision == 0) {
                if (wp_actual >= NUM_WAYPOINTS) {
                    estado_mision = 4; // Misión terminada
                } else {
                    tiempo_objetivo = ruta[wp_actual].metros / VELOCIDAD_MS;
                    temporizador_mision = 0.0;
                    motor_der(3500, 1);
                    motor_izq(3500, 1);
                    led_azul();
                    estado_mision = 1;
                }
            }

            // ESTADO 1: Avanzar hasta cumplir el tiempo calculado
            else if (estado_mision == 1) {
                if (temporizador_mision >= tiempo_objetivo) {
                    detener_motores();

                    // Si el ángulo es 0.0, no giramos. Pasamos al siguiente o terminamos.
                    if (ruta[wp_actual].angulo_giro == 0.0) {
                        wp_actual++;
                        if (wp_actual >= NUM_WAYPOINTS) {
                            estado_mision = 4; // Era el último paso
                        } else {
                            estado_mision = 0; // Seguir con el próximo waypoint
                        }
                    } else {
                        estado_mision = 2; // Ir al giro
                    }
                }
            }

            // ESTADO 2: Arrancar el giro (Detectar Izquierda o Derecha)
            else if (estado_mision == 2) {
                angulo_yaw = 0.0;
                led_rojo();

                if (ruta[wp_actual].angulo_giro < 0) {
                    // Ángulo negativo -> Izquierda
                    motor_der(3500, 1);
                    motor_izq(0, 1);
                }
                else if (ruta[wp_actual].angulo_giro > 0) {
                    // Ángulo positivo -> Derecha
                    motor_izq(3500, 1);
                    motor_der(0, 1);
                }
                estado_mision = 3;
            }

            // ESTADO 3: Esperar a alcanzar los grados ingresados
            else if (estado_mision == 3) {
                // Obtener el valor absoluto del ángulo deseado para la matemática
                float objetivo_grados = ruta[wp_actual].angulo_giro;
                if (objetivo_grados < 0) objetivo_grados = -objetivo_grados;

                // Restar 4 grados para compensar inercia (si el giro es muy pequeño, al menos pedir 1 grado)
                objetivo_grados = objetivo_grados - 4.0;
                if (objetivo_grados < 1.0) objetivo_grados = 1.0;

                // Freno para giro a la Izquierda (el giroscopio sube en positivo)
                if (ruta[wp_actual].angulo_giro < 0 && angulo_yaw >= objetivo_grados) {
                    detener_motores();
                    wp_actual++;
                    estado_mision = 0;
                }
                // Freno para giro a la Derecha (el giroscopio baja en negativo)
                else if (ruta[wp_actual].angulo_giro > 0 && angulo_yaw <= -objetivo_grados) {
                    detener_motores();
                    wp_actual++;
                    estado_mision = 0;
                }
            }

            // ESTADO 4: Misión completada
            else if (estado_mision == 4) {
                detener_motores();
                led_verde();
            }
        }
    }
}


void motores_init(void) {
    SIM->SCGC5 |= 0x0400;
    SIM->SCGC5 |= 0x0800;
    SIM->SCGC6 |= 0x02000000;
    SIM->SOPT2 |= 0x01000000;

    PORTC->PCR[1] = 0x0100; PORTC->PCR[2] = 0x0100;
    PORTC->PCR[3] = 0x0100; PORTC->PCR[4] = 0x0100;
    PTC->PDDR |= (1<<1) | (1<<2) | (1<<3) | (1<<4);
    PTC->PCOR = (1<<1) | (1<<2) | (1<<3) | (1<<4);

    PORTB->PCR[0] = 0x0300;
    PORTB->PCR[1] = 0x0300;

    TPM1->SC = 0;
    TPM1->MOD = 10000;
    TPM1->CONTROLS[0].CnSC = 0x28;
    TPM1->CONTROLS[1].CnSC = 0x28;
    TPM1->CONTROLS[0].CnV = 0;
    TPM1->CONTROLS[1].CnV = 0;
    TPM1->SC = 0x0C;
}

void motor_izq(int velocidad, int adelante) {
    if (velocidad > 10000) velocidad = 10000;
    if (adelante) { PTC->PSOR = (1<<1); PTC->PCOR = (1<<2); }
    else { PTC->PCOR = (1<<1); PTC->PSOR = (1<<2); }
    TPM1->CONTROLS[0].CnV = velocidad;
}

void motor_der(int velocidad, int adelante) {
    if (velocidad > 10000) velocidad = 10000;
    if (adelante) { PTC->PSOR = (1<<3); PTC->PCOR = (1<<4); }
    else { PTC->PCOR = (1<<3); PTC->PSOR = (1<<4); }
    TPM1->CONTROLS[1].CnV = velocidad;
}

void detener_motores(void) {
    PTC->PCOR = (1<<1) | (1<<2) | (1<<3) | (1<<4);
    TPM1->CONTROLS[0].CnV = 0;
    TPM1->CONTROLS[1].CnV = 0;
}

int16_t leer_aceleracion_x(void) {
    unsigned char alto, bajo;
    int16_t valor_x;
    alto = i2c_read_register(MPU9250_ADDR, 0x3B);
    delay_ms(2);
    bajo = i2c_read_register(MPU9250_ADDR, 0x3C);
    valor_x = (int16_t)((alto << 8) | bajo);
    return valor_x;
}

int16_t leer_giroscopio_z(void) {
    unsigned char alto, bajo;
    int16_t valor_z;
    alto = i2c_read_register(MPU9250_ADDR, 0x47);
    delay_ms(2);
    bajo = i2c_read_register(MPU9250_ADDR, 0x48);
    valor_z = (int16_t)((alto << 8) | bajo);
    return valor_z;
}

void pit_init(void) {
    SIM->SCGC6 |= 0x00800000;
    PIT->MCR = 0x00;
    PIT->CHANNEL[0].LDVAL = 524288 - 1;
    PIT->CHANNEL[0].TCTRL = 0x03;
    NVIC->ISER[0] |= (1 << 22);
}

void PIT_IRQHandler(void) {
    PIT->CHANNEL[0].TFLG = 1;
    bandera_20Hz = 1;
}

void i2c_init(void) {
    SIM->SCGC5 |= 0x0800;
    SIM->SCGC4 |= 0x0040;
    PORTC->PCR[8] = 0x0203;
    PORTC->PCR[9] = 0x0203;
    I2C0->F = 0x1F;
    I2C0->C1 = 0x80;
}

unsigned char i2c_read_register(unsigned char dev_addr, unsigned char reg_addr) {
    unsigned char data;
    I2C0->C1 |= 0x30;
    I2C0->D = (dev_addr << 1);
    while((I2C0->S & 0x02) == 0); I2C0->S |= 0x02;
    I2C0->D = reg_addr;
    while((I2C0->S & 0x02) == 0); I2C0->S |= 0x02;
    I2C0->C1 |= 0x04;
    I2C0->D = (dev_addr << 1) | 0x01;
    while((I2C0->S & 0x02) == 0); I2C0->S |= 0x02;
    I2C0->C1 &= ~0x10;
    I2C0->C1 |= 0x08;
    data = I2C0->D;
    while((I2C0->S & 0x02) == 0); I2C0->S |= 0x02;
    I2C0->C1 &= ~0x20;
    data = I2C0->D;
    return data;
}

void delay_ms(int n) {
    int i;
    SysTick->LOAD = 20970 - 1;
    SysTick->CTRL = 0x5;
    for(i = 0; i < n; i++) {
        while((SysTick->CTRL & 0x10000) == 0) {}
    }
    SysTick->CTRL = 0;
}

void led_rgb_init(void) {
    SIM->SCGC5 |= 0x0400;
    SIM->SCGC5 |= 0x1000;
    PORTB->PCR[18] = 0x0100;
    PORTB->PCR[19] = 0x0100;
    PORTD->PCR[1]  = 0x0100;
    PTB->PDDR |= (1<<18) | (1<<19);
    PTD->PDDR |= (1<<1);
    PTB->PSOR = (1<<18) | (1<<19);
    PTD->PSOR = (1<<1);
}

void led_verde(void) { PTB->PSOR = (1<<18); PTD->PSOR = (1<<1); PTB->PCOR = (1<<19); }
void led_azul(void)  { PTB->PSOR = (1<<18); PTB->PSOR = (1<<19); PTD->PCOR = (1<<1); }
void led_rojo(void)  { PTB->PSOR = (1<<19); PTD->PSOR = (1<<1); PTB->PCOR = (1<<18); }

void i2c_write_register(unsigned char dev_addr, unsigned char reg_addr, unsigned char data) {
    I2C0->C1 |= 0x30;
    I2C0->D = (dev_addr << 1);
    while((I2C0->S & 0x02) == 0); I2C0->S |= 0x02;
    I2C0->D = reg_addr;
    while((I2C0->S & 0x02) == 0); I2C0->S |= 0x02;
    I2C0->D = data;
    while((I2C0->S & 0x02) == 0); I2C0->S |= 0x02;
    I2C0->C1 &= ~0x20;
}

void imu_init(void) {
    i2c_write_register(MPU9250_ADDR, 0x6B, 0x80);
    delay_ms(100);
    i2c_write_register(MPU9250_ADDR, 0x6B, 0x01);
    delay_ms(100);
}
