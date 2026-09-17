#define WIN32_LEAN_AND_MEAN
#include <winsock.h>
#include <ws2tcpip.h>
#include <iostream>
#include <string>
#include <cstring>
#include <thread>
#include <mutex>
#include <atomic>
#include <vector>

#pragma comment (lib, "ws2_32.lib")

std::mutex g_log_mtx;
std::atomic<int> g_active{ 0 };


static void log_line(const std::string& s) {
	std::lock_guard<std::mutex> lk(g_log_mtx);
	std::cout << s << '\n';
}
static bool send_all(SOCKET s, const char* data, int len) {
	int total_send = 0;
	while (total_send < len) {
		int k = send(s, data + total_send, len - total_send, 0);
		if (k > 0) {
			total_send += k;
			continue;
		}
		return false;
	}
	return true;
}

static void client_thread(SOCKET s, std::string peer) {
	int now = ++g_active;
	log_line("Client connected " + peer + "(active = " + std::to_string(now) + ")");


	char buf[4096];
	bool alive = true;



	while (alive) {
		int n = recv(s, buf, sizeof(buf), 0);

		if (n > 0) {
			alive = send_all(s, buf, n);
			continue;
		}
		if (n == 0) {
			alive = false;
			break;
		}
		int e = WSAGetLastError();
		if (e == WSAECONNABORTED || e == WSAECONNRESET) {
			alive = false;
			break;
		}
		alive = false;
		break;
	}

	shutdown(s, SD_SEND);
	closesocket(s);

	now = --g_active;
	log_line("Client disconnected: " + peer + "(active = " + std::to_string(now) + ")");

}



int main() {
	WSAData wsa;
	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
		std::cerr << "Error initialization Winsock: " << WSAGetLastError() << '\n';
		return 1;
	}


	SOCKET server = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (server == INVALID_SOCKET) {
		std::cerr << "Error create socket: " << WSAGetLastError() << '\n';
		WSACleanup();
		return 1;
	}

	BOOL yes = TRUE;
	if (setsockopt(server, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&yes), sizeof(yes)) == SOCKET_ERROR) {
		std::cerr << "setsockopt SO_EXCLUSIVEADDRUSE warning : " << WSAGetLastError() << '\n';
	}

	sockaddr_in addr{};

	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = INADDR_ANY;
	addr.sin_port = htons(8080);


	if (bind(server, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
		std::cerr << "Error bind: " << WSAGetLastError() << '\n';
		closesocket(server);
		WSACleanup();
		return 1;
	}


	if (listen(server, SOMAXCONN) == SOCKET_ERROR) {
		std::cerr << "Error listen: " << WSAGetLastError() << '\n';
		closesocket(server);
		WSACleanup();
		return 1;
	}

	std::vector<std::thread> threads;


	while (true) {
		sockaddr_in cli{};
		int len = static_cast<int>(sizeof(cli));
		SOCKET cl = accept(server, reinterpret_cast<sockaddr*>(&cli), &len);
		if (cl == INVALID_SOCKET) {
			std::cerr << "Error accept: " << WSAGetLastError() << '\n';
			break;
		}
		char ip[INET_ADDRSTRLEN];
		if (inet_ntop(AF_INET, &cli.sin_addr, ip, sizeof(ip)) == nullptr) {
			strcpy_s(ip, sizeof(ip), "?");
		}
		std::string peer = std::string(ip) + ":" + std::to_string(ntohs(cli.sin_port));

		try {
			threads.emplace_back(client_thread, cl, peer);
		}
		catch (const std::exception& ex) {
			std::cerr << "Error create thread: " << ex.what() << '\n';
			closesocket(cl);
		}

	}
	closesocket(server);
	for (auto& th : threads) {
		th.join();
	}

	WSACleanup();
	return 0;
}