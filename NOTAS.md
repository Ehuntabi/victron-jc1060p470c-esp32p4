## v1.4.10

Dos arreglos, ambos motivados por el uso real de la app móvil.

`/api/state` solo mandaba la temperatura del congelador (`temp_c`). El
panel físico de Frigo siempre mostró también Aletas y Exterior, pero esas
dos sondas nunca salieron por la API — la app se quedaba forzosamente con
una sola. Ahora `frigo` incluye `temp_aletas_c` y `temp_exterior_c`.

Al navegar por días en los logs (Frigo y Batería), el CSV del día en curso
aparecía dos veces: como "HOY" (RAM + CSV fusionados) y otra vez como
fecha suelta al final de la lista de días de la SD, con los mismos datos.
`log_browser_list_dates` no tenía forma de saber que "HOY" ya cubre esa
fecha; ahora se descarta si coincide con el día de hoy.
