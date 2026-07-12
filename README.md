# Out-of-Core Katz Centrality

Katz centrality на ориентированных графах, которые не влезают в RAM (протестировано: 1.4B рёбер в 128 MB).

## Сборка

```bash
# Ubuntu 22.04
sudo apt-get install -y cmake g++ libomp-dev
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build --parallel

# macOS
brew install cmake libomp
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build --parallel
```

## Запуск

```bash
# 1. Конвертировать CSV → бинарный формат
./build/katz preprocess edges.csv data/out
# Для 1-индексированных графов (SNAP, KONECT):
./build/katz preprocess edges.csv data/out 1

# 2. Отсортировать рёбра
./build/katz sort data/out

# 3. Вычислить Katz centrality
./build/katz katz data/out <alpha> <power|bicgstab> [tol=1e-6] [max_iter=500] [mem_mb=128]
```

Результат: `data/out/katz_scores.csv` (`vertex,rank`).

**Выбор alpha:** безопасно взять `0.85 / d_max`, где `d_max` — максимальная входящая степень (выводится командой `sort`). Подробнее — в `report/report.md`.

## Формат входных данных

CSV, первые две колонки — `from` и `to` (int32). Заголовок опционален, комментарии `#` и `%` игнорируются.

```
from,to
1,2
2,3
3,1
```

## Проверка корректности

```bash
pip install numpy
python3 scripts/verify_katz.py data/out <alpha>
```

## Подробнее

Алгоритм, бенчмарки и анализ — в `report/report.md`.
