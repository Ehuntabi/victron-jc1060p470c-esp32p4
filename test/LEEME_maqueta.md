# Comprobador de maquetación (¿se sale algo de la pantalla?)

`maqueta_hook.c` recorre el árbol de objetos de LVGL en cada pantalla y mide si
algún objeto se sale del panel (1024 × 600). Distingue dos casos:

- **Se sale y está dentro de algo con scroll** → es legítimo: para eso está el
  scroll, el usuario lo alcanza deslizando.
- **Se sale sin scroll** → eso sí es un fallo: queda cortado e inalcanzable.

## Cómo usarlo

1. Copiar el contenido de `maqueta_hook.c` en `main/main.c`, antes de
   `init_camera_and_solar_feed()`.
2. Añadir la tarea junto a las demás, en `app_main()`:
   ```c
   xTaskCreate(maqueta_task, "maqueta", 8192, NULL, 2, NULL);
   ```
3. Compilar, grabar y leer el puerto serie: las líneas llevan la etiqueta
   `MAQUETA`.

## Resultado de la primera pasada (21-sep-2026)

| Pantalla | Objetos | Se salen sin scroll |
|---|---|---|
| pestañas 1 a 5 | 197 | **0** |
| gráfica | 229 | **0** |
| histórico de batería | 23 | **0** |
| histórico solar | 20 | **0** |

Todo lo que sobresale (entre 54 y 126 objetos por pantalla) está **dentro de
contenedores con scroll**, así que se alcanza deslizando: nada queda cortado.

## Limitaciones (honestas)

- Las **pestañas inactivas siguen en el árbol**, así que el recuento de cada
  pestaña incluye el contenido de las demás: el número sirve para detectar
  fallos, no para comparar pantallas entre sí.
- Mide el **contenido que hay en ese momento**. Con datos largos en tiempo de
  ejecución (un SSID largo, un nombre de dispositivo largo) una etiqueta *podría*
  crecer; por eso conviene repetirlo con datos reales.
- Solo cubre las pantallas que se abren por código. **Ajustes, galería, modo
  ausente y los diálogos se abren con el dedo**: ahí la comprobación es mirarlos.
