/* gps.h — u-blox NEO-M9N por UART2.
 *
 * Solo LECTURA de NMEA: se escucha lo que el modulo suelta por su cuenta, sin
 * configurarlo. Con eso hay de sobra para posicion, satelites y hora, y evita
 * depender de comandos UBX que cambian entre versiones de firmware del modulo.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* Llegan tramas del modulo. Distinto de tener posicion: recien encendido y
     * sin ver el cielo, el M9N habla igual pero sin fix. Separarlo permite
     * distinguir "no esta conectado" de "esta buscando", que al diagnosticar es
     * la diferencia entre revisar el cable o esperar. */
    bool     hay_datos;
    bool     hay_fix;          /* posicion valida */
    uint8_t  satelites;        /* usados en la solucion */
    double   lat;              /* grados decimales, + norte */
    double   lon;              /* grados decimales, + este */
    float    altitud_m;
    uint32_t segundos_sin_dato;   /* 0 si acaba de llegar algo */

    /* Fecha y hora UTC tal y como las da el modulo, ya formateadas. Se guardan
     * ademas de usarlas para poner el reloj en hora: verlas en pantalla es lo
     * que confirma de un vistazo que el dato es bueno. Vacias si aun no ha
     * llegado una trama RMC valida. */
    char     fecha[11];           /* "DD-MM-AAAA" */
    char     hora[9];             /* "HH:MM:SS" UTC */
} gps_data_t;

/* Arranca el UART y la tarea de lectura. Una vez, al iniciar. */
void gps_init(void);

/* Copia protegida del estado. */
void gps_get(gps_data_t *out);

/* Ultimas tramas tal y como llegaron, para la pantalla de diagnostico.
 * 'i' va de 0 (la mas antigua guardada) a GPS_CRUDO_N-1. Devuelve "" si esa
 * posicion aun no se ha llenado. */
#define GPS_CRUDO_N   12
void gps_crudo_get(int i, char *out, size_t n);

/* Cuantas veces se ha puesto el reloj en hora con el GPS desde el arranque.
 * Lo enseña el menu: 0 significa que todavia no ha podido. */
uint32_t gps_sincronizaciones(void);

#ifdef __cplusplus
}
#endif
