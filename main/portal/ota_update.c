/* Actualizacion del firmware por Wi-Fi.
 *
 * Para que sirve: hasta ahora, actualizar exigia cable USB y el portatil al lado
 * de la pantalla. Con esto se sube el fichero desde el navegador del movil,
 * estando en la autocaravana.
 *
 * Como funciona: la flash tiene DOS huecos de firmware (ver partitions.csv). Lo
 * que se sube se escribe en el que NO se esta usando, y solo se marca para
 * arrancar si ha llegado entero y valido. Si se corta la subida, se queda todo
 * como estaba: no se puede quedar a medias.
 *
 * Va en su propio fichero y no dentro de config_server.c, que ya tiene ~1.000
 * lineas (1.009 con wc -l el 2-oct-2026).
 */
#include "ota_update.h"
#include <string.h>
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_task_wdt.h"
#include "watchdog.h"
#include "camera.h"
#include "ui.h"
#include "esp_lvgl_port.h"
#include "log_capture/log_capture.h"
#include "display.h"   /* bsp_display_brightness_set/get */
#include "hal/axi_icm_ll.h"  /* QoS del AXI-ICM: prioridad del DMA2D (pantalla) */

static const char *TAG = "ota";

/* Trozo de lectura. 4 KB va sobrado y no come RAM: el cuello de botella es el
 * Wi-Fi, no la flash. */
#define OTA_CHUNK 4096

/* Ademas del watchdog SW propio del proyecto, desuscribe la tarea de camara
 * del Task Watchdog de ESP-IDF mientras dura la OTA (borrado en esp_ota_begin
 * + bucle de escritura): son operaciones de flash largas que pueden monopolizar
 * el nucleo lo suficiente para que cam_stream no llegue a latir en el timeout
 * del TWDT. Reproducido en la placa 2026-08-13: "Reset reason: Watchdog (TASK)",
 * "task_wdt ... cam_stream". watchdog_suspend() NO cubre esto: es un mecanismo
 * de ESP-IDF totalmente aparte del watchdog.c del proyecto. Sin camara
 * arrancada, camera_stream_task_handle() da NULL y esto no hace nada. */
static void ota_wdt_suspend(bool suspend)
{
    watchdog_suspend(suspend);

    /* El borrado de esp_ota_begin (varios segundos) tambien puede impedir
     * que la tarea IDLE del nucleo que ejecuta este worker httpd llegue a
     * latir -- CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0/1=y vigila las dos
     * IDLE con 10s de margen. No es el propio worker el que hay que
     * desuscribir (los workers httpd no se registran en el TWDT por
     * defecto, a diferencia de cam_stream de aqui abajo): es su IDLE. Se
     * desuscriben las DOS a proposito y no solo la del nucleo actual -- el
     * worker puede migrar de nucleo entre el suspend y el resume (no va
     * fijado con core_id), y rastrear cual tocaba en cada momento anadia
     * riesgo de descuadre para una operacion puntual de pocos segundos.
     * Nunca reproducido (a diferencia de cam_stream, que si), pero el
     * margen es real. Detectado el 09-sep-2026. */
    for (int core = 0; core < 2; core++) {
        TaskHandle_t idle = xTaskGetIdleTaskHandleForCore(core);
        if (!idle) continue;
        esp_err_t err_idle = suspend ? esp_task_wdt_delete(idle) : esp_task_wdt_add(idle);
        if (err_idle != ESP_OK) {
            ESP_LOGW(TAG, "TWDT idle nucleo %d: no se pudo %s (%s)",
                     core, suspend ? "desuscribir" : "resuscribir", esp_err_to_name(err_idle));
        }
    }

    /* Y la tarea de LVGL: durante la OTA la pantalla se congela (se coge su lock
     * y no se suelta hasta el final), asi que no puede latir. Sin desuscribirla,
     * el watchdog de tareas reinicia la placa a mitad de la actualizacion: visto
     * en la placa el 6-oct-2026, con el log guardado como log_taskwdt_*. */
    TaskHandle_t lvgl = xTaskGetHandle("taskLVGL");
    if (lvgl) {
        esp_err_t err_lvgl = suspend ? esp_task_wdt_delete(lvgl) : esp_task_wdt_add(lvgl);
        if (err_lvgl != ESP_OK) {
            /* Normal: la tarea de LVGL del port no esta suscrita al TWDT */
            ESP_LOGI(TAG, "TWDT LVGL: no se pudo %s (%s)",
                     suspend ? "desuscribir" : "resuscribir", esp_err_to_name(err_lvgl));
        }
    }

    TaskHandle_t cam = camera_stream_task_handle();
    if (!cam) return;
    esp_err_t err = suspend ? esp_task_wdt_delete(cam) : esp_task_wdt_add(cam);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "TWDT camara: no se pudo %s (%s)",
                 suspend ? "desuscribir" : "resuscribir", esp_err_to_name(err));
    }
}

/* Reinicio diferido: hay que contestar al navegador ANTES de reiniciar, si no el
 * movil se queda esperando y parece que ha fallado. */
static void ota_reboot_cb(void *arg)
{
    (void)arg;
    ESP_LOGW(TAG, "actualizacion aplicada: reiniciando");
    /* Que el arranque siguiente sepa que este reinicio lo pedia una
     * actualizacion: no es una averia y no se apunta como ultimo reinicio. */
    watchdog_marca_reinicio_pedido();
    esp_restart();
}

static void ota_reboot_en(uint32_t ms)
{
    const esp_timer_create_args_t args = {
        .callback = ota_reboot_cb,
        .name = "ota_reboot",
    };
    esp_timer_handle_t t = NULL;
    if (esp_timer_create(&args, &t) == ESP_OK) {
        esp_err_t err = esp_timer_start_once(t, (uint64_t)ms * 1000);
        if (err != ESP_OK) ESP_LOGW(TAG, "timer de reinicio OTA no arranco: %s", esp_err_to_name(err));
    }
}

/* ── Congelar la pantalla mientras se graba la flash ─────────────────────────
 * El parpadeo negro/azul del aviso "Actualizando firmware" no se puede quitar
 * por configuracion en esta placa: el chip de flash es un BOYA y el driver de
 * IDF dice que no soporta flash-suspend
 * (components/spi_flash/spi_flash_chip_boya.c: "flash-suspend is not supported";
 * el chip es mfr 0x68 dev 0x4018, visto con esptool flash_id). Sin suspend, cada
 * borrado/programacion deja al panel sin poder leer su framebuffer.
 *
 * Lo que SI se puede hacer (idea del usuario, 6-oct-2026) es que, mientras se
 * graba, NADA escriba en pantalla: se coge el lock del port LVGL y no se suelta
 * hasta terminar. Asi la tarea de LVGL no corre, no hay redibujados ni cambios
 * de buffer, y el aviso se queda fijo tal cual estaba: lo que se ve es un
 * fotograma congelado, no uno que se reescribe a trozos. Ademas deja de haber
 * reloj/timers tocando el framebuffer justo cuando la flash tiene el bus
 * ocupado. */
static bool s_ui_congelada = false;

static void ota_ui_congelar(void)
{
    /* 1 s de margen: si la tarea de LVGL esta terminando un fotograma, que
     * acabe. Si no se consigue, se sigue igual (peor, pero no se aborta). */
    s_ui_congelada = lvgl_port_lock(1000);
    if (s_ui_congelada) {
        /* El aviso se acaba de crear: refrescar AHORA, con el lock cogido, para
         * que quede PINTADO antes de congelar. Sin esto se congelaba el dibujo
         * anterior y el mensaje no se veia nunca (lo vio el usuario el
         * 6-oct-2026: "sin mensaje de actualizando firmware"). */
        lv_refr_now(NULL);
    }
    ESP_LOGI(TAG, "pantalla congelada durante la grabacion%s",
             s_ui_congelada ? "" : " (NO se pudo coger el lock de LVGL)");
}

static void ota_ui_descongelar(void)
{
    if (!s_ui_congelada) return;
    lvgl_port_unlock();
    s_ui_congelada = false;
    /* La OTA dura ~40 s con la pantalla apagada, asi que el salvapantallas ha
     * podido entrar (en el log se veia "Backlight 25%" justo despues de
     * devolver el brillo, y el "Firmware instalado" salia atenuado). Esto lo
     * despierta y devuelve el brillo del usuario. */
    ui_notify_user_activity();
    ESP_LOGI(TAG, "pantalla descongelada y despertada");
}


/* ── La pantalla, a oscuras mientras se graba la flash ───────────────────────
 * Con la UI congelada el parpadeo no viene de la UI: el panel se queda sin poder
 * leer su framebuffer mientras la flash se borra/escribe (underrun del DSI). En
 * esta placa no hay ajuste que lo quite: el chip de flash es un BOYA y el driver
 * de IDF dice que no soporta flash-suspend, el DPI del P4 no tiene bounce buffer
 * y el framebuffer (1,2 MB) no cabe en RAM interna. Lo unico que queda es que no
 * se vea: se enseña el aviso 3 s con la pantalla encendida y SIN tocar la flash
 * (limpio, sin parpadeo) y despues se apaga el brillo durante la grabacion. El
 * brillo se devuelve en todas las salidas. */
static int s_brillo_previo = -1;

static void ota_pantalla_oscura(void)
{
    if (s_brillo_previo >= 0) return;
    s_brillo_previo = bsp_display_brightness_get();
    bsp_display_brightness_set(0);
    ESP_LOGW(TAG, "pantalla a oscuras durante la grabacion (brillo previo %d%%)",
             s_brillo_previo);
}

/* Vuelve a encender con el brillo que se guardo (sin olvidarlo: el apagado
 * definitivo y la restauracion final lo siguen necesitando). */
static void ota_pantalla_encender(void)
{
    if (s_brillo_previo >= 0) bsp_display_brightness_set(s_brillo_previo);
}

static void ota_pantalla_restaurar(void)
{
    if (s_brillo_previo < 0) return;
    bsp_display_brightness_set(s_brillo_previo);
    ESP_LOGW(TAG, "brillo restaurado al %d%%", s_brillo_previo);
    s_brillo_previo = -1;
}

esp_err_t ota_update_receive(httpd_req_t *req)
{
    const esp_partition_t *destino = esp_ota_get_next_update_partition(NULL);
    if (!destino) {
        ESP_LOGE(TAG, "no hay hueco de actualizacion (reparto de flash antiguo?)");
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req,
            "Esta pantalla no admite actualizacion por Wi-Fi: hay que grabarla "
            "una vez por cable con el reparto de memoria nuevo.");
        return ESP_OK;
    }
    if (req->content_len <= 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "No has adjuntado ningun fichero.");
        return ESP_OK;
    }
    if ((size_t)req->content_len > destino->size) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "El fichero no cabe: no parece un firmware valido.");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "recibiendo %d bytes -> particion '%s'",
             req->content_len, destino->label);

    /* esp_ota_begin borra de golpe el hueco de destino (varios segundos): sin
     * esto el watchdog de UI congelada (watchdog.c) puede saltar y reiniciar
     * la placa a medias, antes de recibir un solo byte. Se reanuda en TODAS
     * las salidas de aqui en adelante, exito o fallo. */
    ota_wdt_suspend(true);
    /* Tapa el parpadeo de pantalla durante la OTA (no lo arregla, ver ui.c).
     * Se oculta en TODAS las salidas de fallo de aqui en adelante; en exito
     * se deja puesto (con el texto cambiado) porque la placa reinicia sola. */
    /* La pantalla se apaga LO PRIMERO: mientras esta encendida ya hay trafico
     * (la subida por Wi-Fi, el refresco del aviso) que le quita PSRAM al panel y
     * se veia parpadear hasta que se ponia negra (lo describio el usuario asi:
     * "empieza parpadeando hasta que se pone negra"). El aviso se sigue pintando
     * (y se congela), pero ya a oscuras: el usuario lo lee en la pagina web y en
     * la pantalla solo ve el "Firmware instalado" del final. */
    /* 1) A oscuras: se pinta el aviso y se congela. El parpadeo del propio
     *    dibujado (copia PSRAM->PSRAM del DMA2D) no se ve. */
    ota_pantalla_oscura();
    ui_ota_overlay_show("Actualizando firmware\n\nNo apagues la pantalla");
    ota_ui_congelar();          /* incluye lv_refr_now: queda PINTADO */
    /* 2) Se enciende: el aviso ya esta completo en el framebuffer y no hay nada
     *    redibujando (la UI esta congelada), asi que se lee LIMPIO, sin
     *    parpadeo. Y todavia no se ha tocado la flash. */
    ota_pantalla_encender();
    vTaskDelay(pdMS_TO_TICKS(3000));
    /* 3) A oscuras otra vez: toda la grabacion (borrado + escritura) se hace sin
     *    que se vea el panel quedarse sin datos. */
    ota_pantalla_oscura();
    /* Deja rastro en la SD de que esta OTA paso por aqui (el log de cada
     * arranque se guarda en el boot; el de esta sesion, si no, se pierde). */
    esp_err_t err_log = log_capture_autosave_now(20);
    ESP_LOGW(TAG, "OTA: pantalla congelada y QoS del DMA2D al maximo, %d bytes (log a SD: %s)",
             req->content_len, esp_err_to_name(err_log));

    esp_ota_handle_t ota = 0;
    esp_err_t err = esp_ota_begin(destino, req->content_len, &ota);
    if (err != ESP_OK) {
        ota_pantalla_restaurar();
        ota_ui_descongelar();
        ui_ota_overlay_hide();
        ota_wdt_suspend(false);
        ESP_LOGE(TAG, "esp_ota_begin: %s", esp_err_to_name(err));
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "No se pudo preparar la actualizacion.");
        return ESP_OK;
    }

    char *buf = malloc(OTA_CHUNK);
    if (!buf) {
        esp_ota_abort(ota);
        ota_pantalla_restaurar();
        ota_ui_descongelar();
        ui_ota_overlay_hide();
        ota_wdt_suspend(false);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "Sin memoria para la actualizacion.");
        return ESP_OK;
    }

    int restante = req->content_len;
    bool fallo = false;
    /* Tope de esperas seguidas. Antes se reintentaba SIN limite: si el movil se
     * iba del Wi-Fi a mitad y el socket se quedaba a medias, esto no salia nunca
     * del bucle y dejaba ocupada la unica tarea del servidor web (con la OTA
     * abierta). Con recv_wait_timeout=30 s, 4 esperas son ~2 min de silencio
     * antes de rendirse. 2026-07-26. */
    const int MAX_ESPERAS = 4;
    /* Ademas del tope de esperas SEGUIDAS, un plazo total absoluto: esperas
     * solo cuenta silencios consecutivos y se reinicia con CUALQUIER byte
     * que llegue, asi que un cliente goteando muy despacio a proposito (1
     * byte cada 25s) nunca disparaba el limite y podia tener esto ocupado
     * sin fin -- la unica tarea del httpd. Firmware entero por Wi-Fi puede
     * tardar de verdad, asi que el plazo es mas generoso que en /save: 10
     * min cubren una subida lenta legitima con margen. Detectado auditando
     * el 08-sep-2026 (mismo motivo que el plazo anadido a /save). */
    const int64_t plazo_us = 10LL * 60 * 1000000LL;
    const int64_t t0_us = esp_timer_get_time();
    int esperas = 0;
    while (restante > 0) {
        if (esp_timer_get_time() - t0_us > plazo_us) {
            ESP_LOGE(TAG, "OTA: plazo total agotado con %d bytes por recibir", restante);
            fallo = true;
            break;
        }
        const int pedir = (restante < OTA_CHUNK) ? restante : OTA_CHUNK;
        const int leido = httpd_req_recv(req, buf, pedir);
        if (leido <= 0) {
            /* HTTPD_SOCK_ERR_TIMEOUT: el movil va lento, se reintenta. */
            if (leido == HTTPD_SOCK_ERR_TIMEOUT && ++esperas <= MAX_ESPERAS) continue;
            if (leido == HTTPD_SOCK_ERR_TIMEOUT) {
                ESP_LOGE(TAG, "la subida lleva %d esperas seguidas sin datos: se corta",
                         esperas);
            }
            ESP_LOGE(TAG, "se corto la subida con %d bytes por recibir", restante);
            fallo = true;
            break;
        }
        esperas = 0;   /* han llegado datos: la cuenta de esperas SEGUIDAS se reinicia */
        if (esp_ota_write(ota, buf, leido) != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write fallo");
            fallo = true;
            break;
        }
        restante -= leido;
        /* Ceder la CPU tras cada chunk (~10 ms/700 chunks ~= 7 s extra en total).
         * Sin esto, con el movil en la misma LAN los datos llegan tan seguidos
         * que httpd_req_recv casi nunca bloquea y el nucleo que atiende esta
         * tarea queda monopolizado por la escritura/borrado de flash: el Task
         * Watchdog de ESP-IDF (distinto del watchdog.c del proyecto, y NO
         * cubierto por watchdog_suspend) puede pillar sin latido incluso a la
         * tarea IDLE de ese nucleo y reiniciar la placa a medias. Reproducido
         * 2026-08-13: "task_wdt ... cam_stream / IDLE0" con Reset reason
         * "Watchdog (TASK)". */
        vTaskDelay(1);
    }
    free(buf);

    if (fallo) {
        esp_ota_abort(ota);
        ota_pantalla_restaurar();
        ota_ui_descongelar();
        ui_ota_overlay_hide();
        ota_wdt_suspend(false);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req,
            "La subida se ha cortado. NO se ha tocado el firmware actual: "
            "la pantalla sigue funcionando igual. Vuelve a intentarlo.");
        return ESP_OK;
    }

    /* esp_ota_end valida la imagen (cabecera y firma del binario). */
    err = esp_ota_end(ota);
    if (err != ESP_OK) {
        ota_pantalla_restaurar();
        ota_ui_descongelar();
        ui_ota_overlay_hide();
        ota_wdt_suspend(false);
        ESP_LOGE(TAG, "esp_ota_end: %s", esp_err_to_name(err));
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req,
            "El fichero no es un firmware valido para esta pantalla. "
            "No se ha cambiado nada.");
        return ESP_OK;
    }

    err = esp_ota_set_boot_partition(destino);
    ota_wdt_suspend(false);
    if (err != ESP_OK) {
        ota_pantalla_restaurar();
        ota_ui_descongelar();
        ui_ota_overlay_hide();
        ESP_LOGE(TAG, "esp_ota_set_boot_partition: %s", esp_err_to_name(err));
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "No se pudo activar la version nueva.");
        return ESP_OK;
    }

    /* Exito: se deja el overlay puesto (con el texto cambiado) hasta que
     * reinicie sola en 1,5 s — no hace falta ocultarlo. El brillo se devuelve
     * antes, para que se lea el "Firmware instalado" (y ya no se toca la flash,
     * asi que esa pantalla sale limpia). */
    ota_pantalla_restaurar();
    ota_ui_descongelar();
    /* Segundo volcado del log, ahora que la grabacion ha terminado: deja en la SD
     * la sesion completa de la OTA (el primero se hace al empezar, para no
     * perderla si algo sale mal). */
    log_capture_autosave_now(20);
    ui_ota_overlay_show("Firmware instalado\n\nReiniciando...");
    ESP_LOGI(TAG, "actualizacion grabada en '%s'", destino->label);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr(req,
        "<!doctype html><meta charset='utf-8'>"
        "<body style='font-family:sans-serif;padding:24px'>"
        "<h2>Actualizacion instalada</h2>"
        "<p>La pantalla se esta reiniciando con la version nueva. "
        "Tardara unos 20 segundos.</p>"
        "<p>Si algo hubiera ido mal, arrancaria sola con la version anterior.</p>"
        "</body>");

    /* 1,5 s para que al movil le de tiempo a recibir la respuesta. */
    ota_reboot_en(1500);
    return ESP_OK;
}

esp_err_t ota_update_page(httpd_req_t *req)
{
    const esp_app_desc_t *app = esp_app_get_description();
    const esp_partition_t *destino = esp_ota_get_next_update_partition(NULL);

    /* El fichero se sube EN CRUDO con fetch(), no con un formulario normal: un
     * formulario lo envuelve con separadores MIME y habria que desenvolverlo en
     * la pantalla (mas codigo y mas cosas que fallen). Asi el servidor solo
     * tiene que escribir lo que recibe. */
    char html[2400];
    snprintf(html, sizeof(html),
        "<!doctype html><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<body style='font-family:sans-serif;padding:24px;max-width:560px'>"
        "<h2>Actualizar la pantalla</h2>"
        "<p>Version instalada: <b>%s</b><br>Compilada el %s</p>"
        "%s"
        "<p><input type='file' id='f' accept='.bin'></p>"
        "<p><button id='b' style='padding:12px 20px;font-size:16px'>Instalar</button></p>"
        "<p id='e'></p>"
        "<p style='background:#fff3cd;border:2px solid #ffb300;padding:12px;"
        "border-radius:8px'><b>No apagues la pantalla durante la actualizacion.</b> "
        "Se pondra <b>negra</b> unos segundos mientras se graba (es normal: asi no "
        "parpadea) y volvera sola. Tarda menos de un minuto.</p>"
        "<p style='color:#666;font-size:14px'>Sube el fichero terminado en "
        "<code>-app.bin</code>. Si se corta, no pasa nada: sigue arrancando la "
        "version actual.</p>"
        "<script>"
        "var b=document.getElementById('b'),f=document.getElementById('f'),e=document.getElementById('e');"
        "b.onclick=function(){"
        " if(!f.files.length){e.textContent='Elige primero el fichero.';return;}"
        " var x=new XMLHttpRequest();b.disabled=true;"
        " x.upload.onprogress=function(p){"
        "  if(p.lengthComputable){e.textContent='Subiendo... '+Math.round(p.loaded*100/p.total)+'%%';}};"
        " x.onload=function(){e.innerHTML=x.responseText;};"
        " x.onerror=function(){e.textContent='Se corto la conexion. La pantalla sigue igual.';b.disabled=false;};"
        " x.open('POST','/ota');"
        " x.setRequestHeader('Content-Type','application/octet-stream');"
        " x.send(f.files[0]);};"
        "</script>"
        "</body>",
        app ? app->version : "?",
        app ? app->date : "?",
        destino ? "" :
            "<p style='color:#b00'><b>Esta pantalla todavia no admite "
            "actualizacion por Wi-Fi.</b> Hay que grabarla una vez por cable con "
            "el reparto de memoria nuevo.</p>");

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr(req, html);
    return ESP_OK;
}
