# Modelo de IA · necesidad de riego

Script de análisis **fuera de línea**: no se ejecuta en el ESP32 ni controla la bomba.

## Instalación

```
pip install -r requirements.txt
```

## Uso

Copia aquí el `agrihome_datos.csv` generado por Node-RED (no se sube al repositorio; se guarda en Drive) y ejecuta:

```
python Agrihome_Modelo_IA_Riego.py                         # división aleatoria 70/30
python Agrihome_Modelo_IA_Riego.py --temporal              # 70 % inicial entrena, 30 % final prueba
python Agrihome_Modelo_IA_Riego.py --csv datos.csv --salida resultados/
```

## Qué hace

1. Limpia el CSV: convierte columnas numéricas, calcula `soil_avg_raw` si falta y descarta filas sin las columnas obligatorias.
2. Etiqueta `requiere_riego = 1` si `soil1_raw > umbral` o `soil2_raw > umbral`.
3. Entrena un `DecisionTreeClassifier(max_depth=4)` con suelo 1, suelo 2, promedio, lux, temperatura, humedad y UV (solo si hay datos UV).
4. Compara el árbol con la **regla de umbral** (línea base) sobre el mismo conjunto de prueba.

## Archivos generados

| Archivo | Contenido |
|---|---|
| `agrihome_resultados_modelo_ia.csv` | Accuracy, precision, recall y F1 del árbol y de la línea base |
| `agrihome_modelo_riego.joblib` | Modelo entrenado y lista de variables |
| `agrihome_matriz_confusion.png` | Matriz de confusión |
| `agrihome_importancia_variables.csv` / `.png` | Importancia de cada variable |
| `agrihome_arbol_decision.png` | Árbol de decisión |

## Limitación importante

La etiqueta se calcula con la misma regla que usa el firmware, así que el árbol aprende a reproducir esa regla y obtiene métricas cercanas al 100 % sin aportar capacidad predictiva propia. Una división aleatoria de muestras tomadas cada segundo infla todavía más las métricas, porque deja muestras casi idénticas en entrenamiento y prueba; por eso existe la opción `--temporal`. Para que el modelo aporte valor hacen falta etiquetas independientes (por ejemplo, observación de la planta o peso del sustrato) y un objetivo predictivo, como anticipar cuándo se necesitará riego.
