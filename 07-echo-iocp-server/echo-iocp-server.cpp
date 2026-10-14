#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>

#include <iostream>
#include <vector>
#include <format>
#include <thread>
#include <unordered_set>
#include <mutex>
#include <atomic>
#include <chrono>
#include <string>




#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "Mswsock.lib")

constexpr int PORT = 5555; // НАШ ПОРТ
constexpr int BACKLOG_POSTED = 64; // количество ACCEPT в полете
constexpr int RECV_BUF = 4096; // 
constexpr unsigned long IDLE_TIMEOUT_MS = 60'000; // Предельное время простоя 
constexpr unsigned long DRAIN_TIMEOUT_MS = 5'000; // Предел ожидания дрежана
constexpr bool VERBOSE = false; // true => логи на каждое соединение

#define LOGV(...) do { if constexpr (VERBOSE) { std::cout << std::format(__VA_ARGS__); } } while(0)



HANDLE g_iocp = NULL; // ЦЕНТР УВЕДОМЛЕНИЙ
SOCKET g_lst = NULL;  // Слушающий сокет

LPFN_ACCEPTEX g_AcceptEx = NULL;                         // УКАЗАТЕЛЬ НА РАСШИРЕННУЮ ФУНКЦИЮ g_AcceptEx
LPFN_GETACCEPTEXSOCKADDRS g_GetAcceptExSockAddrs = NULL; // УКАЗАТЕЛЬ НА РАСШИРЕННУЮ ФУНКЦИЮ g_GetAcceptExSockAddrs

constexpr DWORD LOCALADDRLEN = sizeof(sockaddr) + 16; // LOCALEADDRLEN
constexpr DWORD REMOTEADDRLEN = sizeof(sockaddr) + 16; // REMOTEADDRLEN
constexpr DWORD ACCEPTBUFF = LOCALADDRLEN + REMOTEADDRLEN; // LOCALEADDRLEN + REMOTEADDRLEN

std::atomic<bool> g_stop{ false }; // Запрошена остановка
std::atomic<long> g_liveConns{ 0 }; // Количество живых подключений
std::atomic<long> g_peakConns{ 0 }; // Пиковое количество подключений
std::atomic<long> g_nextId;         // id подключенного
std::atomic<long> g_acceptsInFlyght{ 0 }; // accept-ов в полете

HANDLE g_stopEvent = NULL;

struct alignas(64) Stats {
	unsigned long long accepts = 0;
	unsigned long long recvOps = 0;
	unsigned long long sendOps = 0;
	unsigned long long bytesIn = 0;
	unsigned long long bytesOut = 0;
	unsigned long long closes = 0;
	unsigned long long errors = 0;
	char _pad[64 - (sizeof(unsigned long long) * 7) % 64]{};
};
std::vector<Stats> g_stats;
thread_local Stats* t_st = nullptr;
static Stats aggregate() {
	Stats t;

	for (const Stats& s : g_stats) {
		t.accepts += s.accepts;
		t.bytesIn += s.bytesIn;
		t.bytesOut += s.bytesOut;
		t.closes += s.closes;
		t.errors += s.errors;
		t.recvOps += s.recvOps;
		t.sendOps += s.sendOps;
	}
	return t;
}

static unsigned long long nowMs() { return GetTickCount64(); }


enum class Op { Accept, Recv, Send };


struct IoCtx {
	OVERLAPPED ov;
	Op op;

	IoCtx(Op _op) : ov{}, op(_op) {}
};

struct AcceptCtx : IoCtx {
	SOCKET s = INVALID_SOCKET;
	char buf[ACCEPTBUFF]{};

	AcceptCtx() : IoCtx(Op::Accept) {}
};

struct Conn {
	IoCtx ioRecv{ Op::Recv };
	IoCtx ioSend{ Op::Send };
	SOCKET s = INVALID_SOCKET;
	char buff[RECV_BUF];
	WSABUF wb{};
	DWORD sendTotal = 0;
	DWORD sendsent = 0;
	DWORD flag = 0;

	long id = 0;
	std::atomic<long> ref{ 1 };
	std::atomic<bool> closing{ false };
	std::atomic<unsigned long long>lastAct{ 0 };

	Conn() {
		id = g_nextId.fetch_add(1) + 1;
		long live = g_liveConns.fetch_add(1) + 1;
		long peak = g_peakConns.load();
		while (live > peak && !g_peakConns.compare_exchange_weak(peak, live)) {}
		lastAct.store(nowMs(), std::memory_order_relaxed);

	}
	~Conn() {
		if (s != INVALID_SOCKET) {
			closesocket(s);
			s = INVALID_SOCKET;
		}
		g_liveConns.fetch_sub(1);
	}
	Conn(const Conn&) = delete;
	Conn operator=(const Conn&) = delete;


	void touch() { lastAct.store(nowMs(), std::memory_order_relaxed); }
};

std::mutex g_connsMx; std::unordered_set<Conn*> g_conns;

static void addRef(Conn* c) {
	c->ref.fetch_add(1, std::memory_order_relaxed);
}

static void release(Conn* c) {
	if (c->ref.fetch_sub(1, std::memory_order_acq_rel) == 1) {
		const long id = c->id;
		delete c;
		LOGV("[conn {}] freed (live = {})\n", id, g_liveConns.load());
		if (g_stopEvent) SetEvent(g_stopEvent);
	}
}

static void registryInsert(Conn* c) {
	std::lock_guard lk(g_connsMx);
	g_conns.insert(c);
}
static void closeConn(Conn* c) {
	if (c->closing.exchange(true)) return;
	{
		std::lock_guard lk(g_connsMx);
		g_conns.erase(c);
	}
	if (t_st) ++t_st->closes;

	shutdown(c->s, SD_BOTH);
	CancelIoEx(reinterpret_cast<HANDLE>(c->s), nullptr);

	release(c);
}

static bool postRecv(Conn* c) {
	if (c->closing.load(std::memory_order_acquire)) return false;

	ZeroMemory(&c->ioRecv.ov, sizeof(c->ioRecv.ov));

	c->wb.buf = c->buff;
	c->wb.len = sizeof(c->buff);
	c->flag = 0;

	addRef(c);

	if (WSARecv(c->s, &c->wb, 1, NULL, &c->flag, &c->ioRecv.ov, NULL) == SOCKET_ERROR) {
		int e = WSAGetLastError();
		if (e != WSA_IO_PENDING) {
			if (e != WSAECONNRESET && e != WSAECONNABORTED && e != WSAENOTSOCK && e != WSAEINTR) {
				if (t_st) ++t_st->errors;
				LOGV("Error WSARecv: {}\n", e);
			}
			release(c);
			return false;
		}
	}
	return true;
}

static bool postSend(Conn* c) {
	if (c->closing.load(std::memory_order_acquire)) return false;

	ZeroMemory(&c->ioSend.ov, sizeof(c->ioSend.ov));
	c->wb.buf = c->buff + c->sendsent;
	c->wb.len = c->sendTotal - c->sendsent;

	c->flag = 0;

	addRef(c);

	if (WSASend(c->s, &c->wb, 1, NULL, c->flag, &c->ioSend.ov, NULL) == SOCKET_ERROR) {
		int e = WSAGetLastError();
		if (e != WSA_IO_PENDING) {
			if (e != WSAECONNRESET && e != WSAECONNABORTED && e != WSAENOTSOCK && e != WSAEINTR) {
				if (t_st) ++t_st->errors;
				LOGV("Error WSASend: {}\n", e);
			}
			release(c);
			return false;
		}
	}
	return true;
}

static bool startSend(Conn* c, DWORD bytes) {
	c->sendTotal = bytes;
	c->sendsent = 0;

	return postSend(c);
}

static bool loadExtensions(SOCKET s) {
	GUID gAccept = WSAID_ACCEPTEX;
	GUID gAddrs = WSAID_GETACCEPTEXSOCKADDRS;

	DWORD bytes = 0;

	if (WSAIoctl(g_lst, SIO_GET_EXTENSION_FUNCTION_POINTER, &gAccept, sizeof(gAccept), &g_AcceptEx, sizeof(g_AcceptEx), &bytes, NULL, NULL) == SOCKET_ERROR) {
		std::format("Error EXTENSION(AcceptEx): {}\n", WSAGetLastError());
		return false;
	}
	if (WSAIoctl(g_lst, SIO_GET_EXTENSION_FUNCTION_POINTER, &gAddrs, sizeof(gAddrs), &g_GetAcceptExSockAddrs, sizeof(g_GetAcceptExSockAddrs), &bytes, NULL, NULL) == SOCKET_ERROR) {
		std::format("Error EXTENSION(g_GetAcceptExSockAddrs): {}\n", WSAGetLastError());
		return false;
	}

	return true;
}

static bool postAccept(AcceptCtx* ac) {
	if (g_stop.load(std::memory_order_acquire)) return false;
	ZeroMemory(&ac->ov, sizeof(ac->ov));


	ac->s = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);
	if (ac->s == SOCKET_ERROR) {
		std::cerr << "Error WSASocket: " << WSAGetLastError() << '\n';
		return false;
	}

	g_acceptsInFlyght.fetch_add(1);

	DWORD bytes = 0;
	BOOL ok = g_AcceptEx(g_lst, ac->s, ac->buf, 0, LOCALADDRLEN, REMOTEADDRLEN, &bytes, &ac->ov);

	if (!ok) {
		int e = WSAGetLastError();
		if (e != WSA_IO_PENDING) {
			g_acceptsInFlyght.fetch_sub(1);
			if (!g_stop.load()) std::cerr << std::format("Error AcceptEx: {}\n", e);
			closesocket(ac->s);
			ac->s = INVALID_SOCKET;
			return false;
		}
	}
	return true;
}

static void onAccept(AcceptCtx* ac) {
	if (g_stop.load(std::memory_order_acquire)) return;
	g_acceptsInFlyght.fetch_sub(1);


	SOCKET sock = ac->s;
	ac->s = INVALID_SOCKET;

	if (setsockopt(sock, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT, reinterpret_cast<const char*>(&g_lst), sizeof(g_lst)) == SOCKET_ERROR) {
		LOGV("Error SO_UPDATE_ACCEPT_CONTEXT: {}\n", WSAGetLastError());
		closesocket(sock);
		if (!postAccept(ac)) delete ac;
		return;
	}

	BOOL nodelay = TRUE;
	setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));

	if constexpr (VERBOSE) {
		sockaddr* local = nullptr;
		sockaddr* remote = nullptr;

		int locallen = 0;
		int remotelen = 0;

		g_GetAcceptExSockAddrs(ac->buf, 0, LOCALADDRLEN, REMOTEADDRLEN, &local, &locallen, &remote, &remotelen);
		char ipbuf[INET6_ADDRSTRLEN] = "?";
		unsigned int port;
		if (remote && remote->sa_family == AF_INET) {
			sockaddr_in* r = reinterpret_cast<sockaddr_in*>(remote);
			inet_ntop(AF_INET, &r->sin_addr, ipbuf, sizeof(ipbuf));
			port = ntohs(r->sin_port);
		}
		std::cout << std::format("[conn] connected {}:{}. live conns: {}\n", ipbuf, port, g_liveConns.load());
	}


	Conn* c = new Conn;

	c->s = sock;
	sock = INVALID_SOCKET;

	if (CreateIoCompletionPort(reinterpret_cast<HANDLE>(c->s), g_iocp, reinterpret_cast<ULONG_PTR>(c), 0) == nullptr) {
		LOGV("Error CreateIoCompletionPort: {} \n", WSAGetLastError());
		release(c);
		if (!postAccept(ac)) delete ac;
		return;
	}
	registryInsert(c);

	if (!postRecv(c)) closeConn(c);

	if (!postAccept(ac)) delete ac;


}
static void worker(unsigned idx) {
	t_st = &g_stats[idx];
	for (;;) {
		DWORD bytes = 0;
		ULONG_PTR key = NULL;
		LPOVERLAPPED lp = NULL;


		BOOL ok = GetQueuedCompletionStatus(g_iocp, &bytes, &key, &lp, INFINITE);


		if (!lp) {
			break;
		}

		IoCtx* io = CONTAINING_RECORD(lp, IoCtx, ov);

		if (io->op == Op::Accept) {
			AcceptCtx* ac = static_cast<AcceptCtx*>(io);
			if (!ok) {
				g_acceptsInFlyght.fetch_sub(1);
				if (ac->s != INVALID_SOCKET) {
					closesocket(ac->s);
					ac->s = INVALID_SOCKET;
				}
				if (g_stop.load(std::memory_order_acquire)) {
					delete ac;
					if (g_stopEvent) SetEvent(g_stopEvent);
					continue;
				}
				if (!postAccept(ac)) {
					delete ac;
				}
				continue;
			}
			onAccept(ac);
			continue;
		}


		Conn* c = reinterpret_cast<Conn*>(key);

		if (!ok) {
			closeConn(c);
			release(c);
			continue;
		}
		else if (io->op == Op::Recv) {
			++t_st->recvOps;
			t_st->bytesIn += bytes;
			if (bytes == 0) {
				closeConn(c);
				release(c);
				continue;
			}
			else {
				c->touch();
				if (!startSend(c, bytes)) {
					closeConn(c);
					release(c);
					continue;
				}
			}
		}
		else if (io->op == Op::Send) {
			++t_st->sendOps;
			t_st->bytesOut += bytes;
			if (bytes == 0) {
				closeConn(c);
				release(c);
				continue;
			}
			else {
				c->sendsent += bytes;
				if (c->sendsent < c->sendTotal) {
					c->touch();
					if (!postSend(c)) {
						closeConn(c);
						release(c);
						continue;
					}
				}
				else {
					c->touch();
					if (!postRecv(c)) {
						closeConn(c);
						release(c);
						continue;
					}
				}
			}
		}
		release(c);
	}
}

static void reaper() {

	if (IDLE_TIMEOUT_MS == 0) {
		return;
	}

	for (;;) {

		const DWORD w = WaitForSingleObject(g_stopEvent, 1000);
		if (w != WAIT_TIMEOUT) return;

		const long long now = nowMs();
		std::vector<Conn*> dead;
		{
			std::lock_guard<std::mutex>lk(g_connsMx);
			for (Conn* c : g_conns) {
				const long long last = c->lastAct.load(std::memory_order_relaxed);
				if (now - last >= IDLE_TIMEOUT_MS) {
					addRef(c);
					dead.push_back(c);
				}
			}
		}
		for (Conn* c : dead) {
			LOGV("[conn {}] idle timeout -> close\n", c->id);
			closeConn(c);
			release(c);
		}
	}
}
static void monitor() {
	Stats prev = aggregate();
	auto prevt = std::chrono::steady_clock::now();

	for (;;) {
		const DWORD n = WaitForSingleObject(g_stopEvent, 2000);

		if (n != WAIT_TIMEOUT) {
			return;
		}

		Stats post = aggregate();
		auto nowt = std::chrono::steady_clock::now();
		double dt = std::chrono::duration<double>(nowt - prevt).count();


		if (dt <= 0.0) {
			continue;
		}

		double bytesIn = ((post.bytesIn - prev.bytesIn) / dt) / (1024.0 * 1024.0);
		double bytesOut = ((post.bytesOut - prev.bytesOut) / dt) / (1024.0 * 1024.0);
		double acc = (post.accepts - prev.accepts) / dt;

		std::cout << std::format("[stat] live = {}, peak = {}, acc/s = {}, MB/s in = {}, MB/s out = {}, recv = {}, send = {}, closes = {}, errors = {}\n",
			g_liveConns.load(), g_peakConns.load(), acc, bytesIn, bytesOut, post.recvOps, post.sendOps, post.closes, post.errors);

		prev = post;
		prevt = nowt;
	}
}
static BOOL WINAPI ctrlHandler(DWORD type) {
	switch (type) {
	case CTRL_BREAK_EVENT:
	case CTRL_SHUTDOWN_EVENT:
	case CTRL_C_EVENT:
	case CTRL_CLOSE_EVENT:
		std::cout << "\n[main] shutdown requested\n";
		g_stop.store(true, std::memory_order_release);
		if (g_lst != INVALID_SOCKET) {
			closesocket(g_lst);
			g_lst = INVALID_SOCKET;
		}
		if (g_stopEvent) SetEvent(g_stopEvent);
		return TRUE;
	default:
		return FALSE;
	}

}

int main() {
	WSAData wsa;
	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
		std::cerr << std::format("Error intialization WINSOCK: {}\n", WSAGetLastError());
		return 1;
	}
	g_iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, NULL, 0);
	if (!g_iocp) {
		std::cerr << std::format("Error CreateCompletionPort: {}\n", WSAGetLastError());
		WSACleanup();
		return 1;
	}
	SetConsoleCtrlHandler(ctrlHandler, TRUE);

	g_stopEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
	if (!g_stopEvent) {
		CloseHandle(g_iocp);
		std::cerr << std::format("Error CreateEventW: {}\n", WSAGetLastError());
		WSACleanup();
		return 1;
	}
	g_lst = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);
	if (g_lst == SOCKET_ERROR) {
		CloseHandle(g_iocp);
		CloseHandle(g_stopEvent);
		WSACleanup();
		std::cerr << std::format("Error WSASocket(g_lst): {}", WSAGetLastError());
		return 1;
	}

	BOOL reuse = TRUE;

	setsockopt(g_lst, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));

	sockaddr_in addr{};

	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = INADDR_ANY;
	addr.sin_port = htons(PORT);


	if (bind(g_lst, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
		std::cerr << std::format("Error bind: {}\n", WSAGetLastError());
		CloseHandle(g_iocp);
		CloseHandle(g_stopEvent);
		closesocket(g_lst);
		WSACleanup();
		return 1;
	}

	if (listen(g_lst, SOMAXCONN) == SOCKET_ERROR) {
		std::cerr << std::format("Error listen: {}\n", WSAGetLastError());
		CloseHandle(g_iocp);
		CloseHandle(g_stopEvent);
		closesocket(g_lst);
		WSACleanup();
		return 1;
	}

	if (!loadExtensions(g_lst)) {
		std::cerr << std::format("Error loadExtensions: {}\n", WSAGetLastError());
		CloseHandle(g_iocp);
		CloseHandle(g_stopEvent);
		closesocket(g_lst);
		WSACleanup();
		return 1;
	}

	if (CreateIoCompletionPort(reinterpret_cast<HANDLE>(g_lst), g_iocp, NULL, 0) == NULL) {
		std::cerr << std::format("Error bind listener to IOCP: {}\n", GetLastError());
		CloseHandle(g_iocp);
		CloseHandle(g_stopEvent);
		closesocket(g_lst);
		WSACleanup();
		return 1;
	}

	unsigned hw = std::thread::hardware_concurrency(); // Получаем от ОС какое количество потоков можно запустить

	if (hw == 0) { // Если ОС сказала 0, то ставим = 2
		hw = 2;
	}
	const unsigned nWorkers = hw * 2;
	g_stats.assign(nWorkers, Stats{}); // Делаем размер g_stats = nWorkers и инициализурем Stats нулями.

	std::vector<std::thread> workers; // Создаем вектор потоков workers
	workers.reserve(nWorkers); // делаем размер нашего вектора равным количество потоков nWorkers

	for (unsigned i = 0; i < nWorkers; i++) {
		workers.emplace_back(worker, i);
	}

	std::thread monThread(monitor); // Еще 1 поток для monitor
	std::thread reapThread(reaper); // Еще 1 поток для reaper 

	int posted = 0;


	for (int i = 0; i < BACKLOG_POSTED; i++) {
		AcceptCtx* ac = new AcceptCtx;
		if (!postAccept(ac)) {
			delete ac;                          // Наши BACKLOG_POSTED заготовки 
			continue;
		}
		posted++;
	}
	if (posted == 0) {
		std::cerr << "Error: no AcceptEx posted\n";
		g_stop.store(true);
		SetEvent(g_stopEvent);
	}

	std::cout << std::format("[main] listenin on port = {} , workers = {}, accepts posted = {}\n", PORT, nWorkers, posted);
	std::cout << "Press CTRL+C to shutdown.\n";


	while (!g_stop.load(std::memory_order_acquire)) {  // ЖДЕМ СИГНАЛ ОСТАНОВКИ 
		WaitForSingleObject(g_stopEvent, INFINITE);
	}



	/* --- graceful shutdown --- */
	std::cout << "[main] closing live connections...\n";


	{
		std::vector<Conn*> all;
		{
			std::lock_guard<std::mutex> lk(g_connsMx);
			all.reserve(g_conns.size());
			for (Conn* c : g_conns) { addRef(c); all.push_back(c); }
		}
		for (Conn* c : all) { closeConn(c); release(c); }
	}

	const unsigned long long t0 = nowMs();
	while ((g_liveConns.load() > 0 || g_acceptsInFlyght.load() > 0) &&
		nowMs() - t0 < DRAIN_TIMEOUT_MS) {
		WaitForSingleObject(g_stopEvent, 100);
	}
	if (g_liveConns.load() > 0 || g_acceptsInFlyght.load() > 0) {
		std::cout << std::format("[main] drain timeout: live={} accepts={}\n",
			g_liveConns.load(), g_acceptsInFlyght.load());
	}

	for (unsigned i = 0; i < nWorkers; ++i)
		PostQueuedCompletionStatus(g_iocp, 0, 0, NULL);
	for (std::thread& t : workers)
		if (t.joinable()) t.join();

	SetEvent(g_stopEvent);

	if (monThread.joinable()) monThread.join();

	if (reapThread.joinable()) reapThread.join();

	Stats fin = aggregate();
	std::cout << std::format(
		"\n[main] totals: accepts={} recv={} send={} in={} B out={} B closes={} err={} peak={}\n",
		fin.accepts, fin.recvOps, fin.sendOps,
		fin.bytesIn, fin.bytesOut, fin.closes, fin.errors, g_peakConns.load());

	if (g_lst != INVALID_SOCKET) closesocket(g_lst);

	if (g_iocp) CloseHandle(g_iocp);

	if (g_stopEvent) CloseHandle(g_stopEvent);

	WSACleanup();

	std::cout << "[main] bye\n";
	return 0;
}