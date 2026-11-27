# Protocolo de comunicación · AgriHome (firmware V14.1)

## MQTT

Broker Mosquitto en TCP/1883. Cliente del ESP32: `agrihome-esp32s3-01`. Tamaño máximo de paquete: 1024 bytes.

| Tópico | Dirección | Contenido |
|---|---|---|
| `agrihome/telemetry` | ESP32 → broker | JSON completo cada 1000 ms |
| `agrihome/status` | ESP32 → broker | El mismo JSON más el campo `status` (ACK o error) tras cada comando |
| `agrihome/cmd` | Node-RED / app → ESP32 | Comando de texto o JSON |

El ESP32 reintenta la conexión MQTT cada 5 s y la del Wi-Fi cada 10 s.

### Comandos

| Comando | Acción | Respuesta en `agrihome/status` |
|---|---|---|
| `PUMP_ON` | Enciende la bomba (manual, **sin límite de tiempo** en V14.1) | `ACK PUMP_ON` |
| `PUMP_OFF` | Apaga la bomba y cancela el ciclo automático | `ACK PUMP_OFF` |
| `LEDS_ON` / `LEDS_OFF` | Enciende o apaga los LEDs (el modo autónomo puede volver a imponer su regla) | `ACK LEDS_ON` / `ACK LEDS_OFF` |
| `AUTO_ON` | Activa el modo autónomo y lo guarda | `ACK AUTO_ON` |
| `AUTO_OFF` | Desactiva el modo autónomo, apaga la bomba y lo guarda | `ACK AUTO_OFF` |
| `STATUS` | Pide el estado completo | `ACK STATUS` |
| `{"cmd":"STATUS"}` | Igual que `STATUS` | `ACK STATUS` |
| `{"cmd":"SET_CONFIG", ...}` | Aplica y guarda parámetros | `ACK CONFIG_SAVED` |

Errores: `ERR JSON_INVALID`, `ERR JSON_CMD_UNKNOWN`, `ERR CMD_UNKNOWN`.

### SET_CONFIG

| Campo | Alias corto | Unidad | Límite en firmware |
|---|---|---|---|
| `soil_dry_threshold` | `soil_thr` | RAW (ADC 12 bits) | 0–4095 |
| `lux_low_threshold` | `lux_thr` | lux | 0–10000 |
| `pump_on_time_ms` | `pon` | ms | 100–20000 |
| `pump_cooldown_ms` | `pcd` | ms | 1000–120000 |
| `night_mode_enabled` | `night` | booleano | — |
| `night_start_hour` | `night_start` | h | 0–23 |
| `night_end_hour` | `night_end` | h | 0–23 |
| `current_hour` | `hour` | h | 0–23 |
| `current_minute` | `minute` | min | 0–59 |

Ejemplo:

```json
{"cmd":"SET_CONFIG","soil_dry_threshold":1500,"lux_low_threshold":100,
 "pump_on_time_ms":3000,"pump_cooldown_ms":60000,"night_mode_enabled":true,
 "night_start_hour":20,"night_end_hour":6,"current_hour":12,"current_minute":0}
```

Los parámetros se guardan en memoria no volátil (`Preferences`, espacio de nombres `agrihome`) y se conservan tras reiniciar.

### Campos de telemetría

| Campo | Tipo | Descripción |
|---|---|---|
| `project` | texto | `"Agrihome"` |
| `soil1_raw`, `soil2_raw`, `soil_avg_raw` | entero | Humedad de suelo RAW (mayor = más seco) y promedio |
| `lux` | número | Iluminancia (BH1750) |
| `uv_raw` | entero | Señal analógica del GUVA-S12SD (no es índice UV calibrado) |
| `temperature_c`, `humidity_pct` | número | DHT11 |
| `pump_state`, `leds_state`, `modo_autonomo` | booleano | Actuadores y modo |
| `soil1_dry`, `soil2_dry`, `any_soil_dry` | booleano | RAW > umbral; `any` = OR |
| `low_light`, `auto_pump_running` | booleano | Baja luz; ciclo automático activo |
| `soil_dry_threshold`, `lux_low_threshold`, `pump_on_time_ms`, `pump_cooldown_ms` | número | Parámetros activos |
| `current_hour`, `current_minute` | entero | Reloj manual del ESP32 |
| `night_mode_enabled`, `night_start_hour`, `night_end_hour` | bool / entero | Configuración nocturna |
| `night_time`, `is_night_time`, `leds_blocked_by_night` | booleano | Estado nocturno |
| `wifi_rssi` | entero | dBm |
| `ble_connected`, `mqtt_connected` | booleano | Conectividad |
| `uptime_ms` | entero | Tiempo desde el arranque |
| `status` | texto | Solo en `agrihome/status` |

## Reglas de control (modo autónomo)

- **Riego:** inicia si `soil1_raw > umbral` **o** `soil2_raw > umbral`, dura `pump_on_time_ms` y no vuelve a iniciar hasta que pase `pump_cooldown_ms`, contado desde el fin del ciclo anterior.
- **Luz:** LEDs encendidos si `lux < lux_low_threshold` y no es de noche.
- **Noche:** si inicio < fin, es noche cuando `inicio ≤ hora < fin`; si inicio > fin (por ejemplo, 20 → 6), cuando `hora ≥ inicio` o `hora < fin`. El modo noche solo bloquea los LEDs, no la bomba.

## Bluetooth LE

Nombre: `Agrihome_BLE`. MTU 185. Las características se actualizan cada 1 s desde `loop()` (sin callbacks `onRead`), y los comandos escritos se encolan y procesan fuera del callback.

| Característica | UUID | Propiedad | Contenido |
|---|---|---|---|
| Servicio | `12345678-1234-1234-1234-1234567890ab` | — | — |
| Telemetría | `…890ac` | READ | `{"p":1,"s1","s2","sa","lux","uv","t","h","rssi","up"}` |
| Comandos | `…890ad` | WRITE / WRITE_NR | Mismos comandos que MQTT |
| Estados | `…890ae` | READ | `{"p":2,"pump","leds","auto","d1","d2","dry","low","ap","soil_thr","lux_thr"}` |
| Configuración | `…890af` | READ | `{"p":3,"pon","pcd","hour","minute","night","night_start","night_end","night_time","block_leds"}` |

La app lee las tres características de forma secuencial cada 3 s.
