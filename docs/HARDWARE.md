# Hardware · AgriHome

## Mapa de pines (ESP32-S3 DevKitC-1)

| Elemento | Pin / interfaz | Notas |
|---|---|---|
| Sensor capacitivo de suelo 1 | GPIO 4 (ADC1) | Promedio de 15 lecturas; mayor RAW = más seco |
| Sensor UV GUVA-S12SD | GPIO 5 (ADC1) | Señal relativa, sin calibrar a índice UV |
| Sensor capacitivo de suelo 2 | GPIO 6 (ADC1) | Promedio de 15 lecturas |
| DHT11 | GPIO 7 | Temperatura (°C) y humedad relativa (%) |
| BH1750 | I²C: SDA GPIO 8, SCL GPIO 9 | Iluminancia en lux |
| Bomba (vía driver L298N) | GPIO 12 | Activa en HIGH (`PUMP_ACTIVE_LOW false`) |
| LEDs | GPIO 15 | Activos en HIGH (`LEDS_ACTIVE_LOW false`) |

Los tres sensores analógicos usan el **ADC1**, que es compatible con el Wi-Fi activo (el ADC2 no lo es).

## Precauciones eléctricas

- Los GPIO del ESP32-S3 trabajan a **3.3 V** y no toleran 5 V. Alimenta los sensores a 3.3 V o verifica con multímetro que su salida no supere ese voltaje.
- La bomba y los LEDs de potencia **no** se alimentan desde un GPIO: usan el driver y una fuente externa, con **tierra común** con el ESP32.
- Con una bomba DC, coloca un diodo de protección (flyback) si el driver no lo incluye.
- Mantén la electrónica aislada del agua del riego.

## Pendientes de documentar

- Esquema eléctrico completo, con la fuente externa (voltaje y corriente) y el modelo de la bomba (actividad A04).
- Mediciones de corriente de la bomba y temperatura del L298N, para decidir si se sustituye por un MOSFET de nivel lógico (cambio CH-08).
- Calibración seco/húmedo de cada sensor de suelo (actividad A03).
