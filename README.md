
# winsock-playground

Пошаговое изучение Winsock2: от простейшего блокирующего эхо-сервера
к мультиплексированию через `select()`, пулу потоков и асинхронной модели на IOCP.

Платформа: Windows, MSVC, C++17 (`04-echo-iocp` использует `<format>` → C++20).

## Проекты

| Проект | Модель ввода-вывода | Клиентов одновременно | Порт |
|---|---|---|---|
| `01-echo-blocking` | блокирующие вызовы, один поток | 1 (последовательно) | 8080 |
| `02-echo-select` | `select()`, неблокирующие сокеты, один поток | до `FD_SETSIZE` (1024) | 8080 |
| `03-echo-thread-per-client` | блокирующие сокеты, поток на клиента | ограничено ресурсами ОС | 8080 |
| `04-echo-iocp` | IOCP + `WSARecv`/`WSASend` (overlapped) | тысячи | 9000 |

## 01-echo-blocking

Базовая схема: `socket` → `bind` → `listen` → `accept` → цикл `recv`/`send`.

- Один клиент обслуживается целиком, остальные ждут в очереди `listen(SOMAXCONN)`.
- Отправка циклом по `curr < n`: учтено, что `send()` может передать меньше запрошенного.
- Адрес клиента печатается через `inet_ntop`, при ошибке подставляется `"?"`.
- Завершение: `shutdown(client, SD_SEND)` перед `closesocket`.

Ограничение модели: пока текущий клиент не отключится, другие не обслуживаются.

## 02-echo-select

Однопоточный мультиплексор на `select()`.

- `FD_SETSIZE` переопределяется до 1024 **до** включения `winsock2.h`.
- Все сокеты, включая слушающий, переводятся в неблокирующий режим через `ioctlsocket(FIONBIO)`.
- Состояние клиента — `struct Client { SOCKET s; std::string out; }`: буфер накапливает
  то, что не удалось отправить сразу.
- Backpressure: если `out.size() >= MAX_OUT` (1 MiB), сокет не ставится в `readfds` —
  чтение приостановлено, пока не разгрузится очередь отправки.
- Слушающий сокет добавляется в `readfds` только при `clients.size() + 1 < FD_SETSIZE`.
- `flush_out()` возвращает `false` только на фатальной ошибке; `WSAEWOULDBLOCK` — не ошибка.
- `WSAECONNRESET` / `WSAECONNABORTED` → клиент удаляется без сообщения об ошибке.
- `SO_EXCLUSIVEADDRUSE` защищает порт от перехвата другим процессом.

Ограничение модели: линейное сканирование наборов и жёсткий лимит `FD_SETSIZE`.

## 03-echo-thread-per-client

Классическая схема «поток на подключение».

- Главный поток только принимает соединения и создаёт `std::thread(client_thread, ...)`.
- `send_all()` гарантирует полную отправку принятого блока.
- Логирование через `log_line()` под `std::mutex` — вывод потоков не перемешивается.
- Счётчик активных клиентов — `std::atomic<int> g_active`.
- Исключение при создании потока перехватывается, сокет закрывается.
- Потоки складываются в `std::vector` и присоединяются после выхода из `accept`-цикла.

Ограничения: отдельный стек (~1 MiB) на клиента, переключения контекста;
вектор `threads` растёт неограниченно — завершённые потоки из него не удаляются.

## 04-echo-iocp

Асинхронная модель на порту завершения ввода-вывода.

- Порт создаётся через `CreateIoCompletionPort(INVALID_HANDLE_VALUE, ...)`,
  каждый принятый сокет привязывается к нему с `Conn*` в роли completion key.
- `struct Conn` хранит сокет, буфер 4096 байт, `WSABUF`, `OVERLAPPED`,
  состояние (`Recv` / `Send`) и счётчики `sendTotal` / `sendsent`.
- 4 рабочих потока крутят `GetQueuedCompletionStatus(INFINITE)`.
- Конечный автомат: завершился `Recv` → `startSend()`; завершился `Send` →
  при неполной отправке повторный `postSend()`, иначе новый `postRecv()`.
- `bytes == 0` → клиент закрыл соединение, ресурсы освобождаются.
- Ошибки `WSARecv`/`WSASend`, отличные от `WSA_IO_PENDING`, закрывают соединение.
- `accept`-цикл различает восстановимые (`WSAECONNRESET`, `WSAECONNABORTED`, `WSAEINTR`),
  временные (`WSAEMFILE`, `WSAENOBUFS` — пауза 10 мс) и фатальные ошибки.

Ограничения текущей версии: нет graceful shutdown (рабочие потоки детачатся
accept блокируется навсегда); используется синхронный accept, а не AcceptEx; 
буфер на соединение фиксированный.

## Сборка

Из «x64 Native Tools Command Prompt for VS»:

    cd 01-echo-blocking
    cl /EHsc /std:c++17 server.cpp ws2_32.lib
    cl /EHsc /std:c++17 client.cpp ws2_32.lib

    cd 02-echo-select
    cl /EHsc /std:c++17 echoselect.cpp ws2_32.lib
    cl /EHsc /std:c++17 client\client.cpp ws2_32.lib

    cd 03-echo-thread-per-client
    cl /EHsc /std:c++17 server.cpp ws2_32.lib

    cd 04-echo-iocp
    cl /EHsc /std:c++20 server.cpp ws2_32.lib

Библиотека также подключена через `#pragma comment(lib, "ws2_32.lib")`,
поэтому в Visual Studio достаточно открыть `.sln` / `.slnx`.

## Запуск

Сначала сервер, затем клиент в отдельном окне.
Адрес `127.0.0.1`, порт `8080` (для `04-echo-iocp` — `9000`).

Проверка без клиента, из PowerShell:

    $c = New-Object Net.Sockets.TcpClient('127.0.0.1', 8080)
    $s = $c.GetStream()
    $b = [Text.Encoding]::ASCII.GetBytes("hello`n")
    $s.Write($b, 0, $b.Length)

## Структура

Каждый каталог — независимый проект. Код намеренно не выносится в общую библиотеку:
цель — видеть полный цикл работы с сокетами в одном файле и сравнивать модели напрямую.