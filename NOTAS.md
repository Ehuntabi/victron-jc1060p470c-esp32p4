v4.01 — avisos de la SD a la vista (barra inferior y Ajustes)

## Qué añade

- **Indicador en la barra inferior**, junto a los de Wi-Fi/GPS, siempre visible:
  - **Tarjeta verde** = montada y escribiendo.
  - **Triángulo rojo** = no está montada, o lleva fallos de escritura seguidos.
  - **Tarjeta gris** = la soltaste tú a propósito (Ajustes → Soltar tarjeta).
- **Ajustes → Tarjeta SD**: el texto del problema y qué hacer:
  - "SD: NO montada. Reinicia la pantalla para montarla (no se puede en
    caliente)."
  - "SD: FALLOS al escribir (N seguidos). Puede estar llena, estropeada o con mal
    contacto: NO se está registrando."
  - "SD: soltada a proposito (Ajustes). Reinicia la pantalla para volver a
    montarla."
- Las **capturas de pantalla a medias** ya no se quedan en la tarjeta: si la
  escritura falla, se borra el JPEG truncado (antes quedaba y la galería lo
  enseñaba como bueno).

## Por qué

La SD **no se puede remontar en caliente**: tocar el reloj del controlador SDMMC
con la radio viva rompe el enlace del C6 y esp_hosted reinicia la placa (medido).
Así que si la tarjeta no monta al arrancar, la única salida es reiniciar, y había
que enterarse: antes solo salía un WARN por el puerto serie y el registro (CSV
del frigo, históricos, logs) se perdía en silencio durante toda la sesión.

Por dentro: `datalogger_sd_estado()` cuenta los fallos de escritura **seguidos**
(umbral 3) y distingue "no montó" de "la soltaron a propósito". Los colores siguen
la misma convención que el indicador de GPS de al lado (gris = sin datos,
naranja = a medias, verde = bien).

## Verificado

En la pantalla del banco, forzando los estados desde el firmware de pruebas:
verde → triángulo rojo → verde, visto en directo.
