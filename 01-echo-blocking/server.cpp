
#include <iostream>
#include <string>
#include <WinSock2.h>
#include <WS2tcpip.h>
#pragma comment(lib, "ws2_32.lib")




int main() {

	WSAData wsa;
	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
		std::cerr << "Error initialization Winsock\n";
		return 1;
	}
	SOCKET server = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (server == INVALID_SOCKET) {
		std::cerr << "Error create socket.";
		WSACleanup();
		return 1;
	}
	sockaddr_in addr{};

	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = INADDR_ANY;
	addr.sin_port = htons(8080);

	if (bind(server, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
		std::cerr << "Error bind: " << WSAGetLastError() << "\n";
		closesocket(server);
		WSACleanup();
		return 1;
	}
	if (listen(server, SOMAXCONN) == SOCKET_ERROR) {
		std::cerr << "Error lister: " << WSAGetLastError() << "\n";
		closesocket(server);
		WSACleanup();
		return 1;

	}
	
	std::cout << "Server starting on host 8080.\n";
	char buf[1024];

	while (true) {
		sockaddr_in cli{};
		int len = static_cast<int>(sizeof(cli));
		SOCKET client = accept(server, reinterpret_cast<sockaddr*>(&cli), &len);
		if (client == INVALID_SOCKET) {
			std::cerr << "Error accept: " << WSAGetLastError() << "\n";
			continue;
		}

		char ip[INET_ADDRSTRLEN];
		if (inet_ntop(AF_INET, &cli.sin_addr, ip, sizeof(ip)) == nullptr) {
			strcpy_s(ip, sizeof(ip), "?");
		}
		std::cout << "client with: " << ip << ":" << ntohs(cli.sin_port) << " connected.\n";

		while (true) {
			int n = recv(client, buf, sizeof(buf), 0);
			if (n == 0) {
				std::cout << "Client disconnected.\n";
				break;
			}
			if (n == SOCKET_ERROR) {
				std::cerr << "Error recv " << WSAGetLastError() << "\n";
				break;
			}
			int curr = 0;
			bool ok = true;
		    std::cout << "Client: " << std::string(buf, n);
			while (curr < n ) {
				int k = send(client, buf + curr, n - curr, 0);
				if (k == SOCKET_ERROR) {
					std::cerr << "Error send: " << WSAGetLastError() << "\n";
					ok = false;
					break;
				}
				curr += k;
			}
			if (!ok) { break; }
		}

		std::cout << "Client " << ip << ":" << htons(cli.sin_port) << " disconnected\n";
		shutdown(client, SD_SEND);
		closesocket(client);
	}
	closesocket(server);
	WSACleanup();

	return 0;
}