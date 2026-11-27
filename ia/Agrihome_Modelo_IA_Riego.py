"""
AgriHome · Modelo de IA para necesidad de riego (análisis fuera de línea)

Lee el histórico agrihome_datos.csv generado por Node-RED, entrena un árbol de
decisión y lo compara con la regla de umbral que ya usa el firmware.

IMPORTANTE: la etiqueta `requiere_riego` se calcula con la MISMA regla del
firmware (soil1 > umbral OR soil2 > umbral). Por eso el árbol tiende a
reproducir esa regla y sus métricas no demuestran capacidad predictiva propia.
El modelo NO controla la bomba; es un ejercicio de análisis de datos.

Uso:
    python Agrihome_Modelo_IA_Riego.py                       # división aleatoria 70/30 (original)
    python Agrihome_Modelo_IA_Riego.py --temporal            # 70 % inicial entrena, 30 % final prueba
    python Agrihome_Modelo_IA_Riego.py --csv ruta/al/archivo.csv --salida resultados/
"""
import argparse
import sys
from pathlib import Path

import joblib
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import pandas as pd
from sklearn.metrics import (accuracy_score, classification_report, confusion_matrix,
                             ConfusionMatrixDisplay, f1_score, precision_score, recall_score)
from sklearn.model_selection import train_test_split
from sklearn.tree import DecisionTreeClassifier, plot_tree

OBLIGATORIAS = ["soil1_raw", "soil2_raw", "soil_avg_raw", "lux", "temperature_c", "humidity_pct", "soil_dry_threshold"]
FEATURES_BASE = ["soil1_raw", "soil2_raw", "soil_avg_raw", "lux", "temperature_c", "humidity_pct"]


def cargar(csv: Path) -> pd.DataFrame:
    df = pd.read_csv(csv)
    print(f"Archivo: {csv}  ·  filas: {len(df)}  ·  columnas: {len(df.columns)}")
    print(df.head())
    for c in df.columns:
        if c == "timestamp_iso" or df[c].dtype == bool:
            continue
        df[c] = pd.to_numeric(df[c], errors="coerce")
    if "soil_avg_raw" not in df.columns:
        df["soil_avg_raw"] = (df["soil1_raw"] + df["soil2_raw"]) // 2
    if "soil_dry_threshold" not in df.columns:
        df["soil_dry_threshold"] = 1500
    if "timestamp_iso" in df.columns:
        df["timestamp_iso"] = pd.to_datetime(df["timestamp_iso"], errors="coerce", utc=True)
        df = df.sort_values("timestamp_iso")
    antes = len(df)
    df = df.dropna(subset=OBLIGATORIAS).reset_index(drop=True)
    print(f"Filas descartadas por datos faltantes: {antes - len(df)}")
    return df


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--csv", default="agrihome_datos.csv")
    ap.add_argument("--salida", default=".")
    ap.add_argument("--temporal", action="store_true", help="División temporal en lugar de aleatoria")
    a = ap.parse_args()
    out = Path(a.salida); out.mkdir(parents=True, exist_ok=True)

    df = cargar(Path(a.csv))
    if len(df) < 10:
        print("ADVERTENCIA: la muestra es muy pequeña (< 10 registros).")

    # Etiqueta derivada de la regla del firmware (ver advertencia al inicio)
    df["requiere_riego"] = ((df["soil1_raw"] > df["soil_dry_threshold"]) |
                            (df["soil2_raw"] > df["soil_dry_threshold"])).astype(int)
    print("Distribución de clases:\n", df["requiere_riego"].value_counts().to_string())
    if df["requiere_riego"].nunique() < 2:
        sys.exit("ERROR: la etiqueta tiene una sola clase; no se puede evaluar una clasificación binaria.")

    features = FEATURES_BASE + (["uv_raw"] if "uv_raw" in df.columns and df["uv_raw"].fillna(0).abs().sum() > 0 else [])
    X = df[features].copy()
    X = X.fillna(X.median())
    y = df["requiere_riego"]

    if a.temporal:
        corte = int(len(df) * 0.7)
        X_tr, X_te, y_tr, y_te = X.iloc[:corte], X.iloc[corte:], y.iloc[:corte], y.iloc[corte:]
        modo = "temporal (70 % inicial / 30 % final)"
        if y_te.nunique() < 2 or y_tr.nunique() < 2:
            print("ADVERTENCIA: en la división temporal una parte tiene una sola clase; las métricas serán poco informativas.")
    else:
        X_tr, X_te, y_tr, y_te = train_test_split(X, y, test_size=0.30, random_state=42, stratify=y)
        modo = "aleatoria estratificada 70/30 (random_state=42)"

    modelo = DecisionTreeClassifier(max_depth=4, random_state=42).fit(X_tr, y_tr)
    pred = modelo.predict(X_te)

    # Línea base: la regla del firmware aplicada directamente al conjunto de prueba
    thr = df.loc[X_te.index, "soil_dry_threshold"]
    base = ((X_te["soil1_raw"] > thr) | (X_te["soil2_raw"] > thr)).astype(int)

    def metr(y_pred):
        return {"accuracy": accuracy_score(y_te, y_pred), "precision": precision_score(y_te, y_pred, zero_division=0),
                "recall": recall_score(y_te, y_pred, zero_division=0), "f1": f1_score(y_te, y_pred, zero_division=0)}

    res = pd.DataFrame([{"modelo": "Árbol de decisión (max_depth=4)", **metr(pred)},
                        {"modelo": "Regla de umbral (línea base)", **metr(base)}])
    res.insert(0, "division", modo)
    res.insert(1, "n_entrenamiento", len(X_tr))
    res.insert(2, "n_prueba", len(X_te))
    print("\nDivisión:", modo)
    print(res.to_string(index=False))
    print("\n", classification_report(y_te, pred, zero_division=0))
    res.to_csv(out / "agrihome_resultados_modelo_ia.csv", index=False)

    joblib.dump({"modelo": modelo, "features": features}, out / "agrihome_modelo_riego.joblib")

    cm = confusion_matrix(y_te, pred, labels=[0, 1])
    ConfusionMatrixDisplay(cm, display_labels=["No requiere", "Requiere"]).plot(cmap="viridis")
    plt.title("Matriz de confusión · AgriHome"); plt.tight_layout()
    plt.savefig(out / "agrihome_matriz_confusion.png", dpi=150); plt.close()

    imp = pd.DataFrame({"variable": features, "importancia": modelo.feature_importances_}).sort_values("importancia", ascending=False)
    imp.to_csv(out / "agrihome_importancia_variables.csv", index=False)
    plt.figure(figsize=(7, 4))
    plt.barh(imp["variable"], imp["importancia"], color=plt.cm.tab10.colors[:len(imp)])
    plt.gca().invert_yaxis(); plt.xlabel("Importancia"); plt.title("Importancia de variables")
    plt.tight_layout(); plt.savefig(out / "agrihome_importancia_variables.png", dpi=150); plt.close()

    plt.figure(figsize=(16, 8))
    plot_tree(modelo, feature_names=features, class_names=["No requiere", "Requiere"], filled=True, fontsize=8)
    plt.tight_layout(); plt.savefig(out / "agrihome_arbol_decision.png", dpi=150); plt.close()

    # Predicción de ejemplo (demostrativa; no corresponde a una planta real)
    ej = {"soil1_raw": 2300, "soil2_raw": 2100, "soil_avg_raw": 2200, "lux": 80, "temperature_c": 26.5, "humidity_pct": 55, "uv_raw": 300}
    ej_df = pd.DataFrame([{k: ej[k] for k in features}])
    print("Ejemplo demostrativo:", ej, "→", "Requiere riego" if modelo.predict(ej_df)[0] == 1 else "No requiere riego")
    print(f"\nArchivos generados en: {out.resolve()}")


if __name__ == "__main__":
    main()
