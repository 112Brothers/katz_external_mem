# Out-of-Core Katz Centrality

Katz centrality для ориентированных графов, которые не помещаются в RAM. Протестировано на 1.4B рёбер при ограничении 128 MB.

## Сборка

```bash
# Ubuntu
sudo apt-get install -y cmake g++ libomp-dev

# macOS
brew install cmake libomp
```

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build --parallel
```

## Использование

```bash
# 1. CSV → бинарный формат (добавить 1 для 1-индексированных графов: SNAP, KONECT)
./build/katz preprocess edges.csv data/out [1]

# 2. Сортировка рёбер
./build/katz sort data/out

# 3. Вычисление
./build/katz katz data/out <alpha> <power|bicgstab> [tol=1e-6] [max_iter=500] [mem_mb=128]
```

Результат: `data/out/katz_scores.csv` (колонки `vertex,rank`).

**Выбор alpha:** рекомендуется $\alpha = 0.3/\rho(A)$, где $\rho(A)$ — спектральный радиус матрицы смежности. Стандартное $\alpha = 1/d_{\max}$ даёт слабую дифференциацию вершин. Подробнее — в `report/report.md`.

## Формат входных данных

CSV, первые две колонки — `src` и `dst`. Заголовок опционален, строки с `#` и `%` игнорируются.

## Проверка

```bash
pip install numpy
python3 scripts/verify_katz.py data/out <alpha>
```
