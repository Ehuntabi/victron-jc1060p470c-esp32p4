## v1.4.17

Arreglo de fiabilidad en la actualización por Wi-Fi (`/ota`).

El primer intento de instalar una actualización a veces no hacía nada
visible y la pantalla se reiniciaba sola a medias, sin llegar a escribir el
firmware nuevo. Repitiendo justo después del reinicio sí funcionaba.

Causa: `esp_ota_begin()` borra de golpe el hueco de flash de destino antes
de recibir ningún byte — un bloqueo largo (varios segundos) que hacía
saltar el watchdog interno que vigila si la pantalla se queda congelada.
Ahora la actualización avisa a ese watchdog de que el bloqueo es esperado,
así que ya no hace falta reintentar.
