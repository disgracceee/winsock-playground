
#include <iostream>
#include <string>
#include <WinSock2.h>
#include <WS2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
int main() {
	WSAData wsa;
	if (WSAStartup(MAKEWORD(2, 2), &wsa)) {
		std::cerr << "Error initialization WSAData";
		return 1;
	}

	SOCKET client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (client == INVALID_SOCKET) {
		std::cerr << "Error create socket: " << WSAGetLastError() << "\n";
		WSACleanup();
		return 1;
	}

	sockaddr_in cli{};

	cli.sin_family = AF_INET;
	cli.sin_port = htons(8080);
	inet_pton(AF_INET, "127.0.0.1", &cli.sin_addr);


	if (connect(client, reinterpret_cast<sockaddr*>(&cli), sizeof(cli)) == SOCKET_ERROR) {
		std::cerr << "Error connect: " << WSAGetLastError() << "\n";
		closesocket(client);
		WSACleanup();
		return 1;
	}

	std::cout << "Connected. Type text, empty line = out.\n";
	bool ok = true;
	char buf[1024];
	std::string line;
	while (std::getline(std::cin, line)) {
		if (line.empty()) break;

		int sent = 0; int len = static_cast<int>(line.size());
		while (sent < len) {
			int n = send(client, line.data() + sent, len - sent, 0);
			if (n == SOCKET_ERROR) {
			   std::cerr << "Error send: " << WSAGetLastError() << "\n";
			   ok = false;
			   break;
		    }
			sent += n;
		}
		if (!ok) { break; }
		int n = recv(client, buf, sizeof(buf), 0);
		if (n == 0) { 
			std::cout << "server closed\n";
			break; 
		}
		if (n == SOCKET_ERROR) {
			std::cout << "error recv\n";
			break; 
		}
		std::cout << "Echo: " << std::string(buf, n) << "\n";
	}
	shutdown(client, SD_SEND);
	closesocket(client);
	WSACleanup();

	return 0;
}
