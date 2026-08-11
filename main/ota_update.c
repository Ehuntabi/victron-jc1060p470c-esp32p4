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
 * Va en su propio fichero y no dentro de config_server.c, que ya tiene 2.300
 * lineas.
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
#include "watchdog.h"

static const char *TAG = "ota";

/* Trozo de lectura. 4 KB va sobrado y no come RAM: el cuello de botella es el
 * Wi-Fi, no la flash. */
#define OTA_CHUNK 4096

/* Reinicio diferido: hay que contestar al navegador ANTES de reiniciar, si no el
 * movil se queda esperando y parece que ha fallado. */
static void ota_reboot_cb(void *arg)
{
    (void)arg;
    ESP_LOGW(TAG, "actualizacion aplicada: reiniciando");
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
        esp_timer_start_once(t, (uint64_t)ms * 1000);
    }
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
    watchdog_suspend(true);

    esp_ota_handle_t ota = 0;
    esp_err_t err = esp_ota_begin(destino, req->content_len, &ota);
    if (err != ESP_OK) {
        watchdog_suspend(false);
        ESP_LOGE(TAG, "esp_ota_begin: %s", esp_err_to_name(err));
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "No se pudo preparar la actualizacion.");
        return ESP_OK;
    }

    char *buf = malloc(OTA_CHUNK);
    if (!buf) {
        esp_ota_abort(ota);
        watchdog_suspend(false);
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
    int esperas = 0;
    while (restante > 0) {
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
    }
    free(buf);

    if (fallo) {
        esp_ota_abort(ota);
        watchdog_suspend(false);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req,
            "La subida se ha cortado. NO se ha tocado el firmware actual: "
            "la pantalla sigue funcionando igual. Vuelve a intentarlo.");
        return ESP_OK;
    }

    /* esp_ota_end valida la imagen (cabecera y firma del binario). */
    err = esp_ota_end(ota);
    if (err != ESP_OK) {
        watchdog_suspend(false);
        ESP_LOGE(TAG, "esp_ota_end: %s", esp_err_to_name(err));
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req,
            "El fichero no es un firmware valido para esta pantalla. "
            "No se ha cambiado nada.");
        return ESP_OK;
    }

    err = esp_ota_set_boot_partition(destino);
    watchdog_suspend(false);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition: %s", esp_err_to_name(err));
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "No se pudo activar la version nueva.");
        return ESP_OK;
    }

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
        "<p style='color:#666;font-size:14px'>Sube el fichero terminado en "
        "<code>-app.bin</code> de la version que quieras. No apagues la pantalla "
        "durante la subida. Si se corta, no pasa nada: sigue arrancando la "
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
