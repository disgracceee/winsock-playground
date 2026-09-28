#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>
#include <thread>
#include <vector>
#include <iostream>
#include <format>

#pragma comment(lib, "Ws2_32.lib")
HANDLE g_iocp = NULL;
SOCKET g_lst = INVALID_SOCKET;


LPFN_ACCEPTEX             g_AcceptEx = NULL;
LPFN_GETACCEPTEXSOCKADDRS g_GetAcceptExSockaddrs = NULL;

constexpr DWORD LOCALADDRLEN = sizeof(sockaddr_in) + 16;
constexpr DWORD REMOTEADDRLEN = sizeof(sockaddr_in) + 16;

constexpr DWORD ACCEPTBUF = LOCALADDRLEN + REMOTEADDRLEN;

constexpr int BACKLOG_POSTED = 32;


enum class Op { Recv, Send, Accept };


struct IoCtx {
    OVERLAPPED ov;
    Op op;

    explicit IoCtx(Op o) : op(o) {}
};

struct AcceptCtx : IoCtx {
    SOCKET s = INVALID_SOCKET;
    char buf[ACCEPTBUF];
    AcceptCtx() : IoCtx(Op::Accept) {}
};


struct Conn {
    IoCtx io{ Op::Recv };
    SOCKET s = INVALID_SOCKET;
    char buff[4096];
    WSABUF wb{};
    DWORD sendTotal = 0;
    DWORD sendsent = 0;
    DWORD flags = 0;
};

static void closeConn(Conn* c) {
    if (c->s != INVALID_SOCKET) {
        closesocket(c->s);
        c->s = INVALID_SOCKET;
    }
    delete c;
}

static bool postRecv(Conn* c) {
    ZeroMemory(&c->io.ov, sizeof(c->io.ov));

    c->io.op = Op::Recv;

    c->wb.buf = c->buff;
    c->wb.len = sizeof(c->buff);

    c->flags = 0;
    if (WSARecv(c->s, &c->wb, 1, NULL, &c->flags, &c->io.ov, NULL) == SOCKET_ERROR) {
        int e = WSAGetLastError();
        if (e != WSA_IO_PENDING) {
            if (e != WSAECONNRESET && e != WSAECONNABORTED)
                std::cerr << std::format("Error recv: {}\n", e);
            closeConn(c);
            return false;
        }
    }
    return true;
}


static bool postSend(Conn* c) {
    ZeroMemory(&c->io.ov, sizeof(c->io.ov));
    c->io.op = Op::Send;
    c->wb.buf = c->buff + c->sendsent;
    c->wb.len = c->sendTotal - c->sendsent;

    if (WSASend(c->s, &c->wb, 1, NULL, 0, &c->io.ov, NULL) == SOCKET_ERROR) {
        int e = WSAGetLastError();
        if (e != WSA_IO_PENDING) {
            if (e != WSAECONNRESET && e != WSAECONNABORTED)
                std::cerr << std::format("Error send: {}\n", e);
            closeConn(c);
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

static bool loadExtensions(SOCKET s) {
    GUID gAccept = WSAID_ACCEPTEX;
    GUID gAddrs = WSAID_GETACCEPTEXSOCKADDRS;
    DWORD bytes = 0;

    if (WSAIoctl(s, SIO_GET_EXTENSION_FUNCTION_POINTER, &gAccept, sizeof(gAccept), &g_AcceptEx, sizeof(g_AcceptEx), &bytes, NULL, NULL) == SOCKET_ERROR) {
        std::cerr << std::format("Error EXTENSION FUNC(gAccept): {} \n", WSAGetLastError());
        return false;
    }
    if (WSAIoctl(s, SIO_GET_EXTENSION_FUNCTION_POINTER, &gAddrs, sizeof(gAddrs), &g_GetAcceptExSockaddrs, sizeof(g_GetAcceptExSockaddrs), &bytes, NULL, NULL) == SOCKET_ERROR) {
        std::cerr << std::format("Error EXTENSION FUNC(gAddrs): {}\n", WSAGetLastError());
        return false;
    }
    return true;
}

static bool postAccept(AcceptCtx* s) {
    ZeroMemory(&s->ov, sizeof(s->ov));


    s->s = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);
    if (s->s == INVALID_SOCKET) {
        std::cerr << std::format("Error WSASocket: {}\n", WSAGetLastError());
        return false;
    }


    DWORD bytes = 0;

    BOOL ok = g_AcceptEx(g_lst, s->s, s->buf, 0, LOCALADDRLEN, REMOTEADDRLEN, &bytes, &s->ov);
    int e = WSAGetLastError();
    if (!ok && e != WSA_IO_PENDING) {
        std::cerr << std::format("Error AcceptEx: {}\n", e);
        closesocket(s->s);
        s->s = INVALID_SOCKET;
        return false;
    }
    return true;
}
static void onAccept(AcceptCtx* s) {
    SOCKET sc = s->s;
    s->s = INVALID_SOCKET;

    if (setsockopt(sc, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT, (char*)&g_lst, sizeof(g_lst)) == SOCKET_ERROR) {
        std::cerr << std::format("Error SO_UPDATE_ACCEPT_CONTEXT: {}\n", WSAGetLastError());
        closesocket(sc);
        if (!postAccept(s)) delete s;
        return;
    }
    sockaddr* local = NULL;
    sockaddr* remote = NULL;
    int locallen = 0;
    int remotelen = 0;

    g_GetAcceptExSockaddrs(s->buf, 0, LOCALADDRLEN, REMOTEADDRLEN, &local, &locallen, &remote, &remotelen);

    char ip[INET6_ADDRSTRLEN] = "?";
    unsigned short port = 0;

    if (remote && remote->sa_family == AF_INET) {
        sockaddr_in* temp = reinterpret_cast<sockaddr_in*>(remote);
        inet_ntop(AF_INET, &temp->sin_addr, ip, sizeof(ip));
        port = ntohs(temp->sin_port);
    }

    std::cout << std::format("Client connected: {}:{}\n", ip, port);

    Conn* c = new Conn();

    c->s = sc;
    sc = INVALID_SOCKET;

    if (CreateIoCompletionPort(reinterpret_cast<HANDLE>(c->s), g_iocp, reinterpret_cast<ULONG_PTR>(c), 0) == NULL) {
        std::cerr << std::format("Error CreateCompletionPort: {}\n", GetLastError());
        closeConn(c);
        if (!postAccept(s)) delete s;
        return;
    }
    postRecv(c);
    if (!postAccept(s)) delete s;
}
static void worker() {
    for (;;) {
        DWORD bytes = 0;
        ULONG_PTR key = 0;
        LPOVERLAPPED lp = NULL;

        BOOL ok = GetQueuedCompletionStatus(g_iocp, &bytes, &key, &lp, INFINITE);

        if (!lp) {
            break;
        }

        IoCtx* a = CONTAINING_RECORD(lp, IoCtx, ov);


        if (a->op == Op::Accept) {
            AcceptCtx* ac = static_cast<AcceptCtx*>(a);
            if (!ok) {
                if (ac->s != INVALID_SOCKET) {
                    closesocket(ac->s);
                    ac->s = INVALID_SOCKET;
                }
                postAccept(ac);
                continue;
            }
            onAccept(ac);
            continue;
        }
        Conn* c = reinterpret_cast<Conn*>(key);


        if (!ok) {
            closeConn(c);
            continue;
        }
        if (a->op == Op::Recv) {
            if (bytes == 0) {
                closeConn(c);
                continue;
            }
            startSend(c, bytes);
        }
        else if (a->op == Op::Send) {
            c->sendsent += bytes;
            if (c->sendsent < c->sendTotal) {
                postSend(c);
            }
            else {
                postRecv(c);
            }
        }

    }
}

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

    g_lst = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);
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
    n *= 2;

    std::vector<std::thread> pool;
    pool.reserve(n);
    for (unsigned i = 0; i < n; ++i)
        pool.emplace_back(worker);

    std::cout << std::format("IOCP AcceptEx echo server on 5555, threads: {}, accepts: {}\n",
        n, accepts.size());

    for (auto& t : pool) t.join();
    // // Graceful shutdown добавляется в 07-echo-iocp-server.
    // Здесь воркеры не завершаются, освобождение ресурсов — на ОС.

    closesocket(g_lst);
    CloseHandle(g_iocp);
    WSACleanup();
    return 0;
}