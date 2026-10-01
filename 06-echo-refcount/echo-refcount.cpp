// 06-echo-iocp-refcount
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>
#include <thread>
#include <vector>
#include <atomic>
#include <iostream>
#include <format>
#include <string>

#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "Mswsock.lib")

HANDLE g_iocp = NULL;
SOCKET g_lst = INVALID_SOCKET;

LPFN_ACCEPTEX             g_AcceptEx = NULL;
LPFN_GETACCEPTEXSOCKADDRS g_GetAcceptExSockaddrs = NULL;

constexpr DWORD LOCALADDRLEN = sizeof(sockaddr_in) + 16;
constexpr DWORD REMOTEADDRLEN = sizeof(sockaddr_in) + 16;
constexpr DWORD ACCEPTBUF = LOCALADDRLEN + REMOTEADDRLEN;

constexpr int BACKLOG_POSTED = 32;

// диагностика: видно, что ничего не течёт
std::atomic<long> g_liveConns{ 0 };
std::atomic<long> g_nextId{ 0 };

enum class Op { Recv, Send, Accept };

struct IoCtx {
    OVERLAPPED ov{};
    Op op;

    IoCtx(Op o) : ov{}, op(o) {}      // без explicit: нужно для IoCtx ioRecv{ Op::Recv }
};

struct AcceptCtx : IoCtx {
    SOCKET s = INVALID_SOCKET;
    char buf[ACCEPTBUF]{};

    AcceptCtx() : IoCtx(Op::Accept) {}
};


/* ------------------------------------------------------------------ */
/* Conn + refcount                                                    */
/* ------------------------------------------------------------------ */

struct Conn {
    IoCtx ioRecv{ Op::Recv };
    IoCtx ioSend{ Op::Send };

    SOCKET s = INVALID_SOCKET;
    char buff[4096]{};
    WSABUF wb{};
    DWORD sendTotal = 0;
    DWORD sendsent = 0;
    DWORD flags = 0;

    long id = 0;

    std::atomic<long> ref{ 1 };
    std::atomic<bool> closing{ false };

    Conn() {
        id = g_nextId.fetch_add(1) + 1;
        g_liveConns.fetch_add(1);
    }

    ~Conn() {
        if (s != INVALID_SOCKET) {
            closesocket(s);
            s = INVALID_SOCKET;
        }
        g_liveConns.fetch_sub(1);
    }

    Conn(const Conn&) = delete;
    Conn& operator=(const Conn&) = delete;
};

static void addRef(Conn* c) {
    c->ref.fetch_add(1, std::memory_order_relaxed);
}

static void release(Conn* c) {
    if (c->ref.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        const long id = c->id;
        delete c;
        const long live = g_liveConns.load();
        std::cout << std::format("[conn {}] freed (live={})\n", id, live);
    }
}

static void closeConn(Conn* c) {
    if (c->closing.exchange(true)) return;

    shutdown(c->s, SD_BOTH);
    CancelIoEx(reinterpret_cast<HANDLE>(c->s), nullptr);

    release(c);
}

// ------------------------------------------------------------------ / / post operations / / ------------------------------------------------------------------ /
static bool postRecv(Conn* c) {
    if (c->closing.load(std::memory_order_acquire))
        return false;
    ZeroMemory(&c->ioRecv.ov, sizeof(c->ioRecv.ov));

    c->wb.buf = c->buff;
    c->wb.len = sizeof(c->buff);
    c->flags = 0;

    addRef(c);                           // (2) ссылка на операцию

    if (WSARecv(c->s, &c->wb, 1, NULL, &c->flags, &c->ioRecv.ov, NULL) == SOCKET_ERROR) {
        int e = WSAGetLastError();
        if (e != WSA_IO_PENDING) {
            if (e != WSAECONNRESET && e != WSAECONNABORTED && e != WSAENOTSOCK)
                std::cerr << std::format("Error recv: {}\n", e);
            release(c);                  // пакета в IOCP не будет
            return false;                // closeConn зовёт вызывающий
        }
    }
    return true;
}
static bool postSend(Conn* c) {
    if (c->closing.load(std::memory_order_acquire))
        return false;
    ZeroMemory(&c->ioSend.ov, sizeof(c->ioSend.ov));

    c->wb.buf = c->buff + c->sendsent;
    c->wb.len = c->sendTotal - c->sendsent;

    addRef(c);                           // (2)

    if (WSASend(c->s, &c->wb, 1, NULL, 0, &c->ioSend.ov, NULL) == SOCKET_ERROR) {
        int e = WSAGetLastError();
        if (e != WSA_IO_PENDING) {
            if (e != WSAECONNRESET && e != WSAECONNABORTED && e != WSAENOTSOCK)
                std::cerr << std::format("Error send: {}\n", e);
            release(c);
            return false;
        }
    }
    return true;
}
static bool startSend(Conn* c, DWORD n) {
    c->sendTotal = n;
    c->sendsent = 0;
    return postSend(c);
}
// ------------------------------------------------------------------ / / accept / / ------------------------------------------------------------------ /
static bool loadExtensions(SOCKET s) {
    GUID gAccept = WSAID_ACCEPTEX;
    GUID gAddrs = WSAID_GETACCEPTEXSOCKADDRS;
    DWORD bytes = 0;
    if (WSAIoctl(s, SIO_GET_EXTENSION_FUNCTION_POINTER, &gAccept, sizeof(gAccept),
        &g_AcceptEx, sizeof(g_AcceptEx), &bytes, NULL, NULL) == SOCKET_ERROR) {
        std::cerr << std::format("Error EXTENSION FUNC(gAccept): {}\n", WSAGetLastError());
        return false;
    }
    if (WSAIoctl(s, SIO_GET_EXTENSION_FUNCTION_POINTER, &gAddrs, sizeof(gAddrs),
        &g_GetAcceptExSockaddrs, sizeof(g_GetAcceptExSockaddrs), &bytes, NULL, NULL) == SOCKET_ERROR) {
        std::cerr << std::format("Error EXTENSION FUNC(gAddrs): {}\n", WSAGetLastError());
        return false;
    }
    return true;
}
static bool postAccept(AcceptCtx* a) {
    ZeroMemory(&a->ov, sizeof(a->ov));

    a->s = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED);
    if (a->s == INVALID_SOCKET) {
        std::cerr << std::format("Error WSASocket: {}\n", WSAGetLastError());
        return false;
    }

    DWORD bytes = 0;
    const BOOL ok = g_AcceptEx(g_lst, a->s, a->buf, 0,
        LOCALADDRLEN, REMOTEADDRLEN, &bytes, &a->ov);

    if (!ok) {
        const int e = WSAGetLastError();
        if (e != WSA_IO_PENDING) {
            std::cerr << std::format("Error AcceptEx: {}\n", e);
            closesocket(a->s);
            a->s = INVALID_SOCKET;
            return false;
        }
    }
    return true;
}

static void onAccept(AcceptCtx* a) {
    SOCKET sc = a->s;
    a->s = INVALID_SOCKET;

    if (setsockopt(sc, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT,
        reinterpret_cast<const char*>(&g_lst),
        static_cast<int>(sizeof(g_lst))) == SOCKET_ERROR) {
        std::cerr << std::format("Error SO_UPDATE_ACCEPT_CONTEXT: {}\n", WSAGetLastError());
        closesocket(sc);
        if (!postAccept(a)) delete a;
        return;
    }

    sockaddr* local = nullptr;
    sockaddr* remote = nullptr;
    int locallen = 0, remotelen = 0;

    g_GetAcceptExSockaddrs(a->buf, 0, LOCALADDRLEN, REMOTEADDRLEN,
        &local, &locallen, &remote, &remotelen);

    char ipbuf[INET6_ADDRSTRLEN] = "?";
    unsigned short port = 0;
    if (remote && remote->sa_family == AF_INET) {
        sockaddr_in* temp = reinterpret_cast<sockaddr_in*>(remote);
        inet_ntop(AF_INET, &temp->sin_addr, ipbuf, sizeof(ipbuf));
        port = ntohs(temp->sin_port);
    }
    const std::string ip(ipbuf);          // std::format + char[N] лучше не мешать

    Conn* c = new Conn();
    c->s = sc;

    std::cout << std::format("[conn {}] connected {}:{} (live={})\n",
        c->id, ip, port, g_liveConns.load());

    if (CreateIoCompletionPort(reinterpret_cast<HANDLE>(c->s), g_iocp,
        reinterpret_cast<ULONG_PTR>(c), 0) == nullptr) {
        std::cerr << std::format("Error CreateIoCompletionPort: {}\n", GetLastError());
        closeConn(c);
        if (!postAccept(a)) delete a;
        return;
    }

    if (!postRecv(c)) closeConn(c);

    if (!postAccept(a)) delete a;
}

// ------------------------------------------------------------------ / / worker / / ------------------------------------------------------------------ /
static void worker() {
    for (;;) {
        DWORD bytes = 0;
        ULONG_PTR key = 0;
        LPOVERLAPPED lp = NULL;
        BOOL ok = GetQueuedCompletionStatus(g_iocp, &bytes, &key, &lp, INFINITE);

        if (!lp) break;                  // порт закрыт / стоп-пакет

        IoCtx* a = CONTAINING_RECORD(lp, IoCtx, ov);

        /* ---- accept: свой жизненный цикл, refcount не нужен ---- */
        if (a->op == Op::Accept) {
            AcceptCtx* ac = static_cast<AcceptCtx*>(a);
            if (!ok) {
                if (ac->s != INVALID_SOCKET) {
                    closesocket(ac->s);
                    ac->s = INVALID_SOCKET;
                }
                if (!postAccept(ac)) delete ac;
                continue;
            }
            onAccept(ac);
            continue;
        }

        /* ---- recv/send: объект жив, т.к. на нём висит ссылка (2) ---- */
        Conn* c = reinterpret_cast<Conn*>(key);

        if (!ok) {
            // ошибка или ABORTED от CancelIoEx — просто закрываем
            closeConn(c);
        }
        else if (a->op == Op::Recv) {
            if (bytes == 0) {
                closeConn(c);            // graceful close клиента
            }
            else if (!startSend(c, bytes)) {
                closeConn(c);
            }
        }
        else { // Op::Send
            c->sendsent += bytes;
            if (bytes == 0) {
                closeConn(c);
            }
            else if (c->sendsent < c->sendTotal) {
                if (!postSend(c)) closeConn(c);
            }
            else {
                if (!postRecv(c)) closeConn(c);
            }
        }

        release(c);                      // (3) ВСЕГДА: ссылка операции
    }
}
// ------------------------------------------------------------------ / / main / / ------------------------------------------------------------------ /
int main() {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::cerr << "Error WSAStartup\n";
        return 1;
    }

    g_iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
    if (!g_iocp) {
        std::cerr << std::format("Error CreateIoCompletionPort: {}\n", GetLastError());
        WSACleanup();
        return 1;
    }

    g_lst = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);
    if (g_lst == INVALID_SOCKET) {
        std::cerr << std::format("Error socket: {}\n", WSAGetLastError());
        CloseHandle(g_iocp);
        WSACleanup();
        return 1;
    }

    BOOL yes = TRUE;
    setsockopt(g_lst, SOL_SOCKET, SO_REUSEADDR, (char*)&yes, sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(5555);

    if (bind(g_lst, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        std::cerr << std::format("Error bind: {}\n", WSAGetLastError());
        closesocket(g_lst);
        CloseHandle(g_iocp);
        WSACleanup();
        return 1;
    }

    if (listen(g_lst, SOMAXCONN) == SOCKET_ERROR) {
        std::cerr << std::format("Error listen: {}\n", WSAGetLastError());
        closesocket(g_lst);
        CloseHandle(g_iocp);
        WSACleanup();
        return 1;
    }

    if (!loadExtensions(g_lst)) {
        closesocket(g_lst);
        CloseHandle(g_iocp);
        WSACleanup();
        return 1;
    }

    if (CreateIoCompletionPort((HANDLE)g_lst, g_iocp, 0, 0) == NULL) {
        std::cerr << std::format("Error CreateIoCompletionPort(listen): {}\n", GetLastError());
        closesocket(g_lst);
        CloseHandle(g_iocp);
        WSACleanup();
        return 1;
    }

    std::vector<AcceptCtx*> accepts;
    accepts.reserve(BACKLOG_POSTED);
    for (int i = 0; i < BACKLOG_POSTED; ++i) {
        AcceptCtx* a = new AcceptCtx();
        if (!postAccept(a)) {
            delete a;
            continue;
        }
        accepts.push_back(a);
    }

    if (accepts.empty()) {
        std::cerr << "Error: no accepts posted\n";
        closesocket(g_lst);
        CloseHandle(g_iocp);
        WSACleanup();
        return 1;
    }

    unsigned n = std::thread::hardware_concurrency();
    if (n == 0) n = 2;

    std::vector<std::thread> pool;
    pool.reserve(n);
    for (unsigned i = 0; i < n; ++i)
        pool.emplace_back(worker);

    std::cout << std::format("IOCP refcount echo server on 5555, threads: {}, accepts: {}, live={}\n",
        n, accepts.size(), g_liveConns.load());

    for (auto& t : pool) t.join();
    // // Graceful shutdown добавляется в 07-echo-iocp-server.
    // Здесь воркеры не завершаются, освобождение ресурсов — на ОС.

    closesocket(g_lst);
    CloseHandle(g_iocp);
    WSACleanup();
    return 0;
}