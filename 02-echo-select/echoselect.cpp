#ifndef FD_SETSIZE
#define FD_SETSIZE 1024
#endif
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iostream>
#include <vector>
#include <string>
#include <cstring>

#pragma comment(lib, "ws2_32.lib")

static const std::size_t MAX_OUT = 1 << 20;

struct Client {
	SOCKET s = INVALID_SOCKET;
	std::string out;
};
bool set_noblocking(SOCKET s) {
	u_long mode = 1;
	return ioctlsocket(s, FIONBIO, &mode) == 0;
}

bool flush_out(Client& cl) {
	while (!cl.out.empty()) {
		int n = send(cl.s, cl.out.data(), static_cast<int>(cl.out.size()), 0);
		if (n > 0) {
			cl.out.erase(0, static_cast<size_t>(n));
			continue;
		}
		if (n == 0) {
			break;
		}
		if (n == SOCKET_ERROR) {
			int e = WSAGetLastError();
			if (e == WSAEWOULDBLOCK) {
				return true;
			}
			if (e == WSAECONNRESET || e == WSAECONNABORTED) {
				return false;
			}
			std::cerr << "Error send: " << e << '\n';
			return false;
		}
	}
	return true;
}


int main() {
	WSAData wsa;
	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
		std::cerr << "Error initalization Winsock: " << WSAGetLastError() << '\n';
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
		std::cerr << "Warnings setsockoptr: " << WSAGetLastError() << '\n';
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

	if (!set_noblocking(server)) {
		std::cerr << "Error FIONBIO: " << WSAGetLastError() << '\n';
		closesocket(server);
		WSACleanup();
		return 1;
	}

	char buf[4096];
	std::vector<Client> clients;

	while (true) {
		FD_SET readfds, writefds;

		FD_ZERO(&readfds); FD_ZERO(&writefds);

		bool can_accept = static_cast<long>(clients.size()) + 1 < FD_SETSIZE;
		if (can_accept) {
			FD_SET(server, &readfds);
		}

		for (const Client& c : clients) {
			if (!c.out.empty()) {
				FD_SET(c.s, &writefds);
			}
			if (c.out.size() < MAX_OUT) {
				FD_SET(c.s, &readfds);
			}
		}
		int ready = select(0, &readfds, &writefds, nullptr, nullptr);
		if (ready == SOCKET_ERROR) {
			std::cerr << "Error select: " << WSAGetLastError() << '\n';
			break;
		}
		if (can_accept && FD_ISSET(server, &readfds)) {
			while (true) {
				sockaddr_in cli{};
				int len = (sizeof(cli));
				SOCKET cs = accept(server, reinterpret_cast<sockaddr*>(&cli), &len);
				if (cs == INVALID_SOCKET) {
					int e = WSAGetLastError();
					if (e == WSAEWOULDBLOCK) break;
					std::cerr << "Error accept: " << WSAGetLastError() << '\n';
					break;
				}
				if (static_cast<long>(clients.size()) + 1 >= FD_SETSIZE) {
					closesocket(cs);
					break;
				}
				if (!set_noblocking(cs)) {
					std::cerr << "Error FIONBIO on client: " << WSAGetLastError() << '\n';
					closesocket(cs);
					break;
				}
				char ip[INET_ADDRSTRLEN];
				if (inet_ntop(AF_INET, &cli.sin_addr, ip, sizeof(ip)) == nullptr) {
					strcpy_s(ip, sizeof(ip), "?");
				}
				Client clnt; clnt.s = cs; clients.push_back(std::move(clnt));
				std::cout << "Client connected: " << ip << ":" << ntohs(cli.sin_port) << '\n';
			}
		}
		for (std::size_t i = 0; i < clients.size();) {

			Client& css = clients[i];
			bool alive = true;
			if (FD_ISSET(css.s, &writefds)) {
				alive = flush_out(css);
			}
			if (alive && FD_ISSET(css.s, &readfds)) {
				while (css.out.size() < MAX_OUT) {
					int c = recv(css.s, buf, sizeof(buf), 0);
					if (c > 0) {
						css.out.append(buf, static_cast<std::size_t>(c));
						if (css.out.size() >= MAX_OUT) { break; }
						continue;
					}
					if (c == 0) {
						alive = false;
						break;
					}
					if (c == SOCKET_ERROR) {
						int e = WSAGetLastError();
						if (e == WSAEWOULDBLOCK) {
							break;
						}
						if (e == WSAECONNABORTED || e == WSAECONNRESET) {
							alive = false;
							break;
						}
						std::cerr << "Error recv: " << e << '\n';
						alive = false;
						break;
					}
				}
				if (alive && !css.out.empty()) {
					alive = flush_out(css);
				}
			}
			if (!alive) {
				std::cout << "Client disconnected.\n";
				shutdown(css.s, SD_SEND);
				closesocket(css.s);
				clients.erase(clients.begin() + static_cast<std::ptrdiff_t>(i));
				continue;
			}
			else {
				++i;
			}
		}
	}
	for (Client& cs1 : clients) {
		closesocket(cs1.s);
	}
	closesocket(server);
	WSACleanup();
	return 0;
}
