# Historial de cambios · AgriHome

## [Reorganización del repositorio] · 2026-11-26

- README general, `docs/` (protocolo, hardware y diagrama de integración) e `ia/` (modelo de riego con comparación contra la línea base y opción de división temporal).
- `Agrihome-NodeRed.json`: se sustituye el flujo de 2 nodos por el flujo V15 completo (dashboard, comandos, configuración, registro CSV de 31 columnas con intervalo seleccionable de 1, 30 o 60 s).
- Firmware: las cadenas de versión del Serial y del encabezado dicen V14.1 (antes V13); no cambia el comportamiento.
- App: mensaje de error BLE más claro (antes mencionaba «V12»).
- Se elimina `Agrihome-Platformio/App_agrihome.txt` (copia de `main.dart` con una IP de la red local).
- Se dejan de versionar archivos generados o locales de Flutter (`local.properties`, `Generated.xcconfig`, `ephemeral/`, registrantes de plugins de Android/iOS, `.iml`); se agregan `.gitignore`.

## Firmware V14.1

- Los comandos BLE se copian a `pendingBleCommand` y se procesan en `loop()`, fuera del callback GATT (corrige la inestabilidad al escribir por BLE).
- Las características BLE se actualizan por polling desde `loop()` (sin `onRead`, desde V13).
- Modo noche con intervalo que cruza medianoche; configuración persistente en `Preferences`.

## Planeado · V14.2

- CH-05: apagado automático de `PUMP_ON` manual a los 20 s y prioridad explícita entre control manual y automático.
- CH-06: marcar como falla las lecturas de suelo < 300 o > 3500 RAW y excluirlas de la regla OR.
- CH-07: cooldown en minutos (1–240) con el mismo límite en la app y en el firmware.
