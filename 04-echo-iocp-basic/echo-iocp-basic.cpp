#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <thread>
#include <iostream>
#include <format>

#pragma comment(lib, "Ws2_32.lib")

HANDLE g_iocp = NULL;

struct Conn {
	SOCKET s = INVALID_SOCKET;
	char buff[4096];
	WSABUF wb;
	OVERLAPPED ov;
	enum { Recv, Send } state = Recv;
	DWORD sendTotal = 0;
	DWORD sendsent = 0;
};

void postRecv(Conn* c) {
	ZeroMemory(&c->ov, sizeof(c->ov));

	c->wb.buf = c->buff;
	c->wb.len = sizeof(c->buff);
	c->state = Conn::Recv;

	DWORD flags = 0;
	if (WSARecv(c->s, &c->wb, 1, NULL, &flags, &c->ov, NULL) == SOCKET_ERROR) {
		int e = WSAGetLastError();
		if (e != WSA_IO_PENDING) {
			std::cerr << "Error Recv: " << e << '\n';
			closesocket(c->s);
			delete c;
		}
	}
}

void postSend(Conn* c) {
	ZeroMemory(&c->ov, sizeof(c->ov));

	c->wb.buf = c->buff + c->sendsent;
	c->wb.len = c->sendTotal - c->sendsent;
	c->state = Conn::Send;


	if (WSASend(c->s, &c->wb, 1, NULL, 0, &c->ov, NULL) == SOCKET_ERROR) {
		int e = WSAGetLastError();
		if (e != WSA_IO_PENDING) {
			std::cerr << "Error Send: " << e;
			closesocket(c->s);
			delete c;
		}
	}

}
void startSend(Conn* c, DWORD n) {
	c->sendTotal = n;
	c->sendsent = 0;
	postSend(c);
}

void workers() {
	for (;;) {
		ULONG_PTR key = 0;
		LPOVERLAPPED lp = NULL;
		DWORD bytes = 0;

		BOOL ok = GetQueuedCompletionStatus(g_iocp, &bytes, &key, &lp, INFINITE);

		if (!ok && lp == NULL) {
			continue;
		}

		Conn* c = reinterpret_cast<Conn*>(key);

		if (!ok) {
			DWORD e = GetLastError();
			std::cerr << "Error IO: " << e << '\n';
			closesocket(c->s);
			delete c;
			continue;
		}

		if (bytes == 0) {
			std::cerr << "Closed.\n";
			closesocket(c->s);
			delete c;
			continue;
		}

		if (c->state == Conn::Recv) {
			startSend(c, bytes);
		}
		else {
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
	WSAData wsa;

	int r = WSAStartup(MAKEWORD(2, 2), &wsa);
	if (r != 0) {
		std::cerr << "Error initialization Winsock: " << r << '\n';
		return 1;
	}
	g_iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
	if (!g_iocp) {
		DWORD e = GetLastError();
		std::cerr << "Error iocp: " << e << '\n';
		WSACleanup();
		return 1;
	}

	for (std::size_t i = 0; i < 4; i++) {
		std::thread(workers).detach();
	}

	SOCKET lst = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (lst == INVALID_SOCKET) {
		int e = WSAGetLastError();
		std::cerr << "Error create socket: " << e << '\n';
		WSACleanup();
		return 1;
	}


	sockaddr_in addr{};

	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = INADDR_ANY;
	addr.sin_port = htons(9000);

	BOOL yes = TRUE;

	if (setsockopt(lst, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&yes), sizeof(yes)) == SOCKET_ERROR) {
		std::cerr << "setsockopt warning: " << WSAGetLastError() << '\n';
	}

	if (bind(lst, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
		int e = WSAGetLastError();
		std::cerr << "Error bind: " << e << '\n';
		closesocket(lst);
		WSACleanup();
		return 1;
	}

	if (listen(lst, SOMAXCONN) == SOCKET_ERROR) {
		int e = WSAGetLastError();
		std::cerr << "Error listen: " << e << '\n';
		closesocket(lst);
		WSACleanup();
		return 1;
	}
	std::cerr << "Listening on 0.0.0.0:9000\n";
	for (;;) {

		SOCKET s = accept(lst, NULL, NULL);
		if (s == INVALID_SOCKET) {
			int e = WSAGetLastError();
			if (e == WSAECONNRESET || e == WSAECONNABORTED || e == WSAEINTR) {
				std::cerr << "Error accept: " << e << '\n';
				continue;
			}
			if (e == WSAEMFILE || e == WSAENOBUFS) {
				Sleep(10);
				continue;
			}
			std::cerr << "Closed(Fatal error: " << e << ")\n";
			closesocket(lst);
			WSACleanup();
			return 0;
		}
		Conn* c = new Conn();

		c->s = s;

		if (!CreateIoCompletionPort(reinterpret_cast<HANDLE>(s), g_iocp, reinterpret_cast<ULONG_PTR>(c), 0)) {
			std::cerr << std::format("Error iocp bind: {}\n", GetLastError());
			closesocket(s);
			delete c;
			continue;
		}
		std::cerr << "Accept\n";
		postRecv(c);
	}


	closesocket(lst);
	WSACleanup();
	return 0;
}
