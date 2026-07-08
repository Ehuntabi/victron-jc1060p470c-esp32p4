#pragma once

/* Visor de galeria EN PANTALLA: navega las capturas guardadas en la SD y las
 * muestra a pantalla completa (overlay). Se abre desde Settings->Display.
 * Paso 1: capturas del carrusel (BMP en /sdcard/screenshots).
 * Paso 2 (pendiente): vigilancia (JPG en /sdcard/vigilancia) via decoder HW. */
void ui_gallery_open(void);
