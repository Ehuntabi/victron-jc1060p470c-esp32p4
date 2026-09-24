/* simulador.c — Emisor de datos de prueba hacia la P4 (tanda BLE).
 *
 * QUE HACE
 *   Emite, rotando, los SEIS tipos de registro que la P4 sabe descifrar, con
 *   valores que se mueven (para que se vean las tarjetas, las graficas y los
 *   contadores de energia) y, cada cierto tiempo, valores de aviso y centinelas
 *   de "sin dato" (0x7FFF / 0xFFFF), para ejercitar tambien esos caminos.
 *
 *   Un mismo aparato emitiendo todos los tipos no es realista (un Victron
 *   manda el suyo), pero es justo lo que se quiere aqui: que la P4 reciba de
 *   todo. Va documentado asi en el LEEME.
 *
 * FORMATOS (sacados del parser de la P4, victron_ble.c). Los campos van en
 * little-endian y a partir del byte 8 algunos van EMPAQUETADOS A BITS:
 *
 *   0x01 SmartSolar   12 B: estado, error, V(2), I(2), yield(2), PV(2), carga(2: 9 bits)
 *   0x02 Monitor bat. 15 B: TTG(2) V(2) alarma(2) aux(2) | 2+22+20+10 bits
 *   0x03 Inversor     11 B: estado, alarma(2), V(2), VA(2), 15+11 bits
 *   0x04 DC/DC        10 B: estado, error, Vin(2), Vout(2), off_reason(4)
 *   0x05 Litio        17 B: flags(4), error(2), 8 celdas, V+balanceo(2), temp(1)
 *   0x0F Orion XS     14 B: estado, error, Vout(2), Iout(2), Vin(2), Iin(2), off(4)
 */
#include "simulador.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/aes.h"

#include "nimble/nimble_port.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "host/ble_gap.h"

static const char *TAG = "simulador";

#define MAX_DISPOS 4
#define TIPOS 6

static const uint8_t TIPO_REGISTRO[TIPOS] = { 0x01, 0x02, 0x03, 0x04, 0x05, 0x0F };
static const uint16_t PRODUCTO[TIPOS] = {
    0xA042,   /* BlueSolar MPPT 75/15 */
    0xA389,   /* SmartShunt 500A/50mV */
    0xA231,   /* Phoenix Inverter 12V 250VA */
    0xA3D0,   /* Orion-Tr Smart DC-DC */
    0x0000,   /* litio: la lista de la P4 no tiene ese producto (sale "(unknown)") */
    0xA3D0,   /* Orion XS (el mas parecido de la lista) */
};

typedef struct {
    uint8_t  mac[6];
    uint8_t  clave[16];
    char     mac_txt[18];
} dispos_t;

static dispos_t s_disp[MAX_DISPOS];
static int      s_ndisp = 0;
static volatile bool s_activo = false;
static volatile bool s_silencio = false;   /* ver sim_silencio() */
static TaskHandle_t s_task = NULL;

/* Contadores del informe */
static uint32_t s_enviados[TIPOS];
static uint32_t s_total = 0;
static uint16_t s_nonce = 1;
static volatile bool s_extremo = false;
static volatile bool s_caos = false;
static volatile int  s_ritmo_ms = 200;
static volatile int  s_fase_caos = 0;
static volatile bool s_fijo = false;
static int s_fijo_soc = 637, s_fijo_v = 1337, s_fijo_i = -4444;

/* Onda triangular de -amplitud..+amplitud con periodo 40 pasos, para que los
 * valores se muevan sin depender de la libreria de matematicas. */
static int onda(int fase, int amplitud)
{
    int p = fase % 40;
    if (p > 20) p = 40 - p;
    return (p - 10) * amplitud / 10;
}

/* Construye el registro en claro. Devuelve el numero de bytes. */
static int construir(int tipo, int fase, bool aviso, uint8_t *out)
{
    memset(out, 0, 32);

    if (s_fijo && tipo == 1) {
        /* Valores fijos y reconocibles: 63,7 % | 13,37 V | -4,444 A */
        out[0] = 0xFF; out[1] = 0x00;               /* TTG */
        out[2] = s_fijo_v & 0xFF; out[3] = (s_fijo_v >> 8) & 0xFF;
        uint64_t cola = 0;
        cola |= ((uint64_t)(s_fijo_i & 0x3FFFFF)) << 2;
        cola |= ((uint64_t)(s_fijo_soc & 0x3FF)) << 44;
        for (int b = 0; b < 7; b++) out[8 + b] = (uint8_t)((cola >> (8 * b)) & 0xFF);
        return 15;
    }

    if (s_extremo) {
        /* Todos los bytes a valores limite: 0x7FFF/0xFFFF son los centinelas de
         * "sin dato" del formato Victron, y el resto son maximos. */
        switch (tipo) {
        case 0:   /* solar: 12 B */
            memset(out, 0xFF, 12);
            out[2] = 0xFF; out[3] = 0x7F;          /* V = NA */
            out[4] = 0xFF; out[5] = 0x7F;          /* I = NA */
            out[10] = 0xFF; out[11] = 0x01;        /* carga = 0x1FF = NA */
            return 12;
        case 1:   /* monitor de bateria: V=NA, SOC=1023 (102,3 %) */
            out[0] = 0xFF; out[1] = 0xFF;          /* TTG = 0xFFFF */
            out[2] = 0xFF; out[3] = 0x7F;          /* V = NA */
            {
                uint64_t cola = 0;
                cola |= ((uint64_t)0x3FFFFF) << 2;          /* corriente al maximo */
                cola |= ((uint64_t)0xFFFFF) << 24;          /* consumo al maximo */
                cola |= ((uint64_t)0x3FF) << 44;            /* SOC = 1023 */
                for (int b = 0; b < 7; b++) out[8 + b] = (uint8_t)((cola >> (8*b)) & 0xFF);
            }
            return 15;
        case 2:   /* inversor: V=0x7FFF, alarma llena, VCA al maximo */
            memset(out, 0xFF, 11);
            out[3] = 0xFF; out[4] = 0x7F;
            return 11;
        case 3:   /* DC/DC: Vin = 0xFFFF (NA), Vout = 0x7FFF (NA) */
            memset(out, 0xFF, 10);
            out[2] = 0xFF; out[3] = 0xFF;
            out[4] = 0xFF; out[5] = 0x7F;
            return 10;
        case 4:   /* litio: celdas a 255, temperatura 255 (-40 = 215 C) */
            memset(out, 0xFF, 17);
            return 17;
        default:  /* Orion XS */
            memset(out, 0xFF, 14);
            return 14;
        }
    }
    int v = 1320 + onda(fase, 60);            /* 12,60 - 13,80 V */
    int i = -4200 + onda(fase + 7, 2500);     /* -6,7 - -1,7 A */
    int soc = aviso ? 220 : 780 + onda(fase, 200);   /* decimas de % */
    int ttg = aviso ? 35 : 240 + onda(fase + 3, 90);

    switch (tipo) {
    case 0:   /* 0x01 SmartSolar, 12 bytes */
        out[0] = aviso ? 3 : 4;               /* estado */
        out[1] = 0;
        out[2] = v & 0xFF; out[3] = (v >> 8) & 0xFF;
        out[4] = (uint8_t)(int8_t)(i / 100); out[5] = (uint8_t)((i / 100) >> 8);
        { uint16_t y = (uint16_t)(120 + onda(fase, 60)); out[6] = y & 0xFF; out[7] = y >> 8; }
        { uint16_t pv = (uint16_t)(180 + onda(fase + 5, 120)); out[8] = pv & 0xFF; out[9] = pv >> 8; }
        { uint16_t carga = (uint16_t)(20 + onda(fase + 2, 15)); out[10] = carga & 0xFF; out[11] = (carga >> 8) & 0x01; }
        return 12;

    case 1: { /* 0x02 monitor de bateria, 15 bytes, cola empaquetada a bits */
        out[0] = ttg & 0xFF; out[1] = (ttg >> 8) & 0xFF;
        out[2] = v & 0xFF;   out[3] = (v >> 8) & 0xFF;
        out[4] = 0; out[5] = 0;               /* alarma */
        out[6] = 0; out[7] = 0;               /* auxiliar */
        uint64_t cola = 0;                    /* bits 0-1: entrada auxiliar = 0 */
        cola |= ((uint64_t)(i & 0x3FFFFF)) << 2;
        /* bits 24-43: consumo (0), bits 44-53: SOC */
        cola |= ((uint64_t)(soc & 0x3FF)) << 44;
        for (int b = 0; b < 7; b++) out[8 + b] = (uint8_t)((cola >> (8 * b)) & 0xFF);
        return 15;
    }

    case 2: { /* 0x03 inversor, 11 bytes */
        out[0] = 3;
        uint16_t alarma = aviso ? 0x0004 : 0;
        out[1] = alarma & 0xFF; out[2] = alarma >> 8;
        out[3] = v & 0xFF; out[4] = (v >> 8) & 0xFF;
        uint16_t va = (uint16_t)(600 + onda(fase, 400));
        out[5] = va & 0xFF; out[6] = va >> 8;
        uint32_t cola = (uint32_t)(23000 + onda(fase + 1, 500)) & 0x7FFF;   /* 230,0 V */
        cola |= ((uint32_t)((20 + onda(fase + 4, 15)) & 0x7FF)) << 15;      /* 2,0 A */
        out[7] = cola & 0xFF; out[8] = (cola >> 8) & 0xFF;
        out[9] = (cola >> 16) & 0xFF; out[10] = (cola >> 24) & 0xFF;
        return 11;
    }

    case 3:   /* 0x04 DC/DC, 10 bytes */
        out[0] = 4; out[1] = 0;
        out[2] = 1380 & 0xFF; out[3] = 1380 >> 8;
        out[4] = 1320 & 0xFF; out[5] = 1320 >> 8;
        out[6] = 0; out[7] = 0; out[8] = 0; out[9] = 0;
        return 10;

    case 4: { /* 0x05 litio, 17 bytes */
        uint32_t flags = aviso ? 0x00000001 : 0;
        out[0] = flags & 0xFF; out[1] = (flags >> 8) & 0xFF;
        out[2] = (flags >> 16) & 0xFF; out[3] = (flags >> 24) & 0xFF;
        out[4] = 0; out[5] = 0;
        for (int c = 0; c < 8; c++) out[6 + c] = (uint8_t)(33 + (c % 3));  /* celdas */
        uint16_t empaq = (uint16_t)(v & 0x0FFF) | (uint16_t)((aviso ? 3 : 0) << 12);
        out[14] = empaq & 0xFF; out[15] = empaq >> 8;
        out[16] = (uint8_t)(60 + onda(fase, 8));   /* crudo: la P4 le resta 40 */
        return 17;
    }

    default: /* 0x0F Orion XS, 14 bytes */
        out[0] = 4; out[1] = 0;
        out[2] = (1320 + onda(fase, 40)) & 0xFF; out[3] = ((1320 + onda(fase, 40)) >> 8) & 0xFF;
        { uint16_t io = (uint16_t)(150 + onda(fase + 2, 100)); out[4] = io & 0xFF; out[5] = io >> 8; }
        out[6] = 1380 & 0xFF; out[7] = 1380 >> 8;
        { uint16_t ii = (uint16_t)(160 + onda(fase + 6, 90)); out[8] = ii & 0xFF; out[9] = ii >> 8; }
        out[10] = 0; out[11] = 0; out[12] = 0; out[13] = 0;
        return 14;
    }
}

/* Cifra y monta los datos de fabricante. */
static int montar_trama(int tipo, int fase, bool aviso, const dispos_t *d, uint8_t *mfg)
{
    uint8_t claro[32];
    int len = construir(tipo, fase, aviso, claro);

    uint8_t cifrado[32] = {0};
    uint8_t ctr[16] = { (uint8_t)(s_nonce & 0xFF), (uint8_t)(s_nonce >> 8) };
    uint8_t stream[16] = {0};
    size_t offset = 0;
    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    if (mbedtls_aes_setkey_enc(&aes, d->clave, 128) != 0) { mbedtls_aes_free(&aes); return 0; }
    int rc = mbedtls_aes_crypt_ctr(&aes, (size_t)len, &offset, ctr, stream, claro, cifrado);
    mbedtls_aes_free(&aes);
    if (rc != 0) return 0;

    mfg[0] = 0xE1; mfg[1] = 0x02;
    mfg[2] = 0x10;
    mfg[3] = (uint8_t)(1 + 2 + 1 + 2 + 1 + len);
    mfg[4] = PRODUCTO[tipo] & 0xFF; mfg[5] = PRODUCTO[tipo] >> 8;
    mfg[6] = TIPO_REGISTRO[tipo];
    mfg[7] = (uint8_t)(s_nonce & 0xFF);
    mfg[8] = (uint8_t)(s_nonce >> 8);
    mfg[9] = d->clave[0];
    memcpy(&mfg[10], cifrado, (size_t)len);
    s_nonce++;
    return 10 + len;
}

/* Trama malformada a proposito: identificador de fabricante que no es Victron,
 * tipo de registro desconocido, longitudes raras y bytes al azar. Es para
 * comprobar que la P4 se defiende (no debe reiniciarse ni publicar basura). */
static void emitir_fuzz(int fase, const dispos_t *d)
{
    uint8_t mfg[48];
    int n = 10 + (fase * 7) % 20;
    if (n > 40) n = 40;
    for (int k = 0; k < n; k++) mfg[k] = (uint8_t)(fase * 31 + k * 17);
    mfg[0] = (fase & 1) ? 0xE1 : 0x99;      /* a veces ni siquiera es Victron */
    mfg[1] = 0x02;
    mfg[2] = 0x10;
    mfg[3] = (uint8_t)n;
    mfg[6] = (uint8_t)(fase & 0xFF);        /* tipo de registro al azar */
    mfg[9] = (fase & 2) ? d->clave[0] : (uint8_t)(d->clave[0] ^ 0xFF);  /* clave a veces mal */

    ble_gap_adv_stop();
    if (ble_hs_id_set_rnd(d->mac) != 0) return;
    struct ble_hs_adv_fields campos = {0};
    campos.mfg_data = mfg;
    campos.mfg_data_len = (uint8_t)n;
    if (ble_gap_adv_set_fields(&campos) != 0) return;
    struct ble_gap_adv_params prm = {0};
    prm.conn_mode = BLE_GAP_CONN_MODE_NON;
    prm.disc_mode = BLE_GAP_DISC_MODE_GEN;
    if (ble_gap_adv_start(BLE_OWN_ADDR_RANDOM, NULL, BLE_HS_FOREVER, &prm, NULL, NULL) == 0) {
        s_total++;
    }
}

static void emitir(int tipo, int fase, bool aviso, const dispos_t *d)
{
    uint8_t mfg[48];
    int n = montar_trama(tipo, fase, aviso, d, mfg);
    if (n <= 0) return;

    ble_gap_adv_stop();
    if (ble_hs_id_set_rnd(d->mac) != 0) return;

    struct ble_hs_adv_fields campos = {0};
    campos.mfg_data = mfg;
    campos.mfg_data_len = (uint8_t)n;
    if (ble_gap_adv_set_fields(&campos) != 0) return;

    struct ble_gap_adv_params prm = {0};
    prm.conn_mode = BLE_GAP_CONN_MODE_NON;
    prm.disc_mode = BLE_GAP_DISC_MODE_GEN;
    if (ble_gap_adv_start(BLE_OWN_ADDR_RANDOM, NULL, BLE_HS_FOREVER, &prm, NULL, NULL) == 0) {
        s_enviados[tipo]++;
        s_total++;
    }
}

static void tarea_sim(void *arg)
{
    (void)arg;
    int fase = 0;
    int tipo = 0;
    int dev = 0;
    /* Cada 61 tramas, una de AVISO (SOC bajo, estado de alarma, error). 61 y 6
     * son primos entre si, asi que el aviso cae en un tipo de registro distinto
     * cada vez: con 60 (multiplo de 6) le tocaba SIEMPRE al mismo y la alarma de
     * bateria no se probaba nunca. Fallo mio, visto el 24-sep-2026 con el banco:
     * la P4 jamas activaba MINI_ALARM_BATERIA porque nunca le llegaba un SOC bajo. */
    int aviso_cada = 61;
    int contador = 0;

    int caos_reloj = 0;
    while (s_activo) {
        bool aviso = (aviso_cada > 0) && (++contador % aviso_cada == 0);

        /* Modo caos: cada ~600 tramas (2 min al ritmo normal) cambia de fase. */
        if (s_caos && (++caos_reloj % 600) == 0) {
            s_fase_caos = (s_fase_caos + 1) & 3;
            s_fijo    = (s_fase_caos == 1);
            s_extremo = (s_fase_caos == 2);
            printf("CAOS: ahora en fase %s\n", sim_ble_modo());
        }

        /* Una sola trama de aviso dura ~50 ms y la P4 evalua las alarmas cada
         * 500 ms, asi que muchas veces no la pilla. El aviso va en racha de 20
         * tramas (~1 s), que es lo que hace que la alarma salte de verdad. */
        bool en_racha_aviso = (contador % (aviso_cada * 20)) >= (aviso_cada * 20 - 20);
        bool aviso_ahora = aviso || (en_racha_aviso && tipo == 1);

        if (s_caos && s_fase_caos == 3) emitir_fuzz(fase, &s_disp[dev]);
        else                            emitir(tipo, fase, aviso_ahora, &s_disp[dev]);

        if (++tipo >= TIPOS) { tipo = 0; if (++dev >= s_ndisp) dev = 0; fase++; }

        /* Cada 30 s (150 tramas), un resumen que el PC pueda leer del puerto y
         * cruzar con lo que la P4 dice que ha descifrado. */
        if ((s_total % 150) == 0 && !s_silencio) {
            printf("RESUMEN ble=%u solar=%u bat=%u inv=%u dcdc=%u litio=%u orion=%u modo=%s\n",
                   (unsigned)s_total, (unsigned)s_enviados[0], (unsigned)s_enviados[1],
                   (unsigned)s_enviados[2], (unsigned)s_enviados[3],
                   (unsigned)s_enviados[4], (unsigned)s_enviados[5], sim_ble_modo());
        }
        vTaskDelay(pdMS_TO_TICKS(s_ritmo_ms));
    }
    ble_gap_adv_stop();
    s_task = NULL;
    vTaskDelete(NULL);
}

bool sim_ble_iniciar(int n, const char *macs[], const char *claves[])
{
    if (n < 1 || n > MAX_DISPOS) return false;
    if (s_activo) sim_ble_parar();

    s_ndisp = 0;
    for (int k = 0; k < n; k++) {
        /* MAC "AA:BB:CC:DD:EE:FF" -> orden NimBLE (al reves) */
        int v[6];
        if (sscanf(macs[k], "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6)
            return false;
        for (int j = 0; j < 6; j++) s_disp[k].mac[j] = (uint8_t)v[5 - j];
        snprintf(s_disp[k].mac_txt, sizeof(s_disp[k].mac_txt), "%s", macs[k]);

        if (strlen(claves[k]) != 32) return false;
        for (int j = 0; j < 16; j++) {
            unsigned byte;
            if (sscanf(claves[k] + 2 * j, "%2x", &byte) != 1) return false;
            s_disp[k].clave[j] = (uint8_t)byte;
        }
        s_ndisp++;
    }

    memset(s_enviados, 0, sizeof(s_enviados));
    s_total = 0;
    s_nonce = 1;
    s_activo = true;
    xTaskCreate(tarea_sim, "sim_ble", 4096, NULL, 3, &s_task);
    return true;
}

void sim_ble_parar(void)
{
    s_activo = false;
    vTaskDelay(pdMS_TO_TICKS(400));
}

bool sim_ble_activo(void) { return s_activo; }
void sim_silencio(bool on) { s_silencio = on; }

void sim_ble_fijo(bool activar, int soc_deci, int v_centi, int i_milli)
{
    s_fijo = activar;
    if (soc_deci >= 0)   s_fijo_soc = soc_deci;
    if (v_centi > 0)     s_fijo_v = v_centi;
    s_fijo_i = i_milli;
}

void sim_ble_ritmo(int ms) { if (ms >= 10 && ms <= 5000) s_ritmo_ms = ms; }
void sim_ble_caos(bool activar) { s_caos = activar; }

const char *sim_ble_modo(void)
{
    if (s_caos) {
        static const char *nombres[4] = { "caos:normal", "caos:fijos", "caos:extremos", "caos:fuzz" };
        return nombres[s_fase_caos & 3];
    }
    if (s_extremo) return "extremos";
    if (s_fijo)    return "fijos";
    return "normal";
}

void sim_ble_extremo(bool activar) { s_extremo = activar; }
bool sim_ble_extremo_activo(void) { return s_extremo; }

void sim_ble_informe(void)
{
    static const char *nombres[TIPOS] = {
        "SmartSolar", "Monitor bat.", "Inversor", "DC/DC", "Litio", "Orion XS" };
    printf("SIM BLE %s, %d dispositivo(s), %u tramas\n",
           s_activo ? "EMITIENDO" : "parado", s_ndisp, (unsigned)s_total);
    for (int k = 0; k < TIPOS; k++) {
        if (s_enviados[k]) printf("   %-13s %u\n", nombres[k], (unsigned)s_enviados[k]);
    }
    for (int k = 0; k < s_ndisp; k++) {
        printf("   aparato %d: %s\n", k + 1, s_disp[k].mac_txt);
    }
}
