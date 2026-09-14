\# Winsock Playground



Учебные реализации TCP эхо-сервера на Windows (Winsock2).



\## Содержание



| Папка | Модель | Клиентов одновременно |

|---|---|---|

| `01-echo-blocking` | блокирующие сокеты, один поток | 1 |



\## Сборка



Developer Command Prompt for VS, из папки `01-echo-blocking`:



# winsock-playground

Пошаговое изучение Winsock2: от простейшего блокирующего сервера
к мультиплексированию и асинхронным моделям.

Платформа: Windows, MSVC, C++17.

## Проекты

| Проект | Модель ввода-вывода | Клиентов одновременно |
|---|---|---|
| `01-echo-blocking` | блокирующие вызовы, один сокет | 1 |
| `02-echo-select` | `select()`, один поток | до `FD_SETSIZE` (1024) |

## Сборка

Из «x64 Native Tools Command Prompt for VS»:

```
cd 01-echo-blocking
cl /EHsc /std:c++17 server.cpp ws2_32.lib
cl /EHsc /std:c++17 client.cpp ws2_32.lib
```

```
cd 02-echo-select
cl /EHsc /std:c++17 echoselect.cpp ws2_32.lib
cl /EHsc /std:c++17 client\client.cpp ws2_32.lib
```

Либо открыть `.sln` / `.slnx` в Visual Studio.

## Запуск

Сначала сервер, затем клиент в отдельном окне.
Адрес `127.0.0.1`, порт `8080`.

## Структура

Каждый каталог — независимый проект со своим README,
где описаны детали реализации и ограничения модели.
