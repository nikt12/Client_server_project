#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <pthread.h>
#include <fcntl.h>
#include "crc.h"
#include "protocol.h"

extern errno;

#define EPOLL_QUEUE_LEN 100		//длина очереди событий epoll
#define MAX_EPOLL_EVENTS 100	//максимальное число событий epoll за одну итерацию
#define EPOLL_RUN_TIMEOUT -1	//таймаут ожидания событий (-1 соответствует ожиданию до первого наступившего события)

//прототип функции создания и связывания сокета
int listener(const char *port, const char *transport, const char *qlen);

void eventLoopUDP(int serverSock, connection connList[]);

void eventLoopTCP(int serverSock, connection connList[]);

//прототип функции разбора подстрок строки по полям структуры
int deSerializer(connection *connList, int currentConn, char *buffer);

//прототип функции сбора сегментов строки в единую строку
void Assembler(struct epoll_event evList[], int currItem, char *buffer, int serverSock, struct sockaddr_in clientAddr);

//прототип функции установки блокирующего/неблокирующего режима работы с файловым дескриптором
int fd_set_blocking(int fd, int blocking);

int main(int argc, char *argv[]) {
	int serverSock;							//дескриптор сокета сервера
	connection connList[NUM_OF_CONNECTIONS];			//массив структур с данными о клиентах и соединений с ними

	//инициализируем используемые строки и структуры нулями
	memset(&connList, 0, sizeof(connList));

	//проверяем, получено ли необходимое количество аргументов
	if (argc == 4) {
		//создаем сокет сервера и выполняем его привязку с переводом в режим прослушивания
		serverSock = listener(argv[1], argv[2], argv[3]);
		//проверяем значение дескриптора сокета
		if (serverSock < 0)
			return -1;
		else {
			printf("Ожидание подключений...\n");
			if((strcmp(argv[2], "tcp") == 0) || (strcmp(argv[2], "TCP") == 0))
				eventLoopTCP(serverSock, connList);
			else
				eventLoopUDP(serverSock, connList);
		}
	}
	else
		//если введено неверное количество аргументов, выводим правильный формат запуска программы
		printf("Использование: %s port transport query_length\n", argv[0]);
	return 0;
}

//реализация функции создания и связывания сокета
//аргументы:
//port - порт, с которым связывается сервер
//transport - протокол, по которому будет работать сервер
//qlen - длина очереди на подключение к сокету
int listener(const char *port, const char *transport, const char *qlen) {
	struct sockaddr_in sin;			//структура IP-адреса
	int s, type, proto, q_len;				//дескриптор и тип сокета

	q_len = atoi(qlen);
	//обнуляем структуру адреса
	memset(&sin, 0, sizeof(sin));
	//указываем тип адреса
	sin.sin_family = AF_INET;
	//указываем в качестве адреса шаблон INADDR_ANY, т.е. все сетевые интерфейсы
	sin.sin_addr.s_addr = htonl(INADDR_ANY);
	//конвертируем номер порта из пользовательского порядка байт в сетевой
	sin.sin_port = htons((unsigned short)atoi(port));

	//используем имя протокола для определения типа сокета
	if(strcmp(transport, "udp") == 0) {
		type = SOCK_DGRAM;					//UDP
		proto = IPPROTO_UDP;
	}
	else if(strcmp(transport, "tcp") == 0) {
		type = SOCK_STREAM;					//TCP
		proto = IPPROTO_TCP;
	}
	else {
		printf("Некооректно указан транспортный протокол.\n");
		return -1;
	}

	//вызываем функцию создания сокета
	s = socket(PF_INET, type, proto);

	//проверяем правильность создания
	if (s < 0) {
		printf("Ошибка создания сокета: %s.\n", strerror(errno));
		return -1;
	}

	//переводим созданный сокет в неблокирующий режим для корректной работы epoll
	fd_set_blocking(s, 0);

	//привязка сокета с проверкой результата
	if(bind(s, (struct sockaddr *)&sin, sizeof(sin)) < 0) {
		printf("Ошибка связывания сокета: %s.\n", strerror(errno));
		return -1;
	}

	if(type == SOCK_STREAM)
		//запуск прослушивания с проверкой результата
		if (listen(s, q_len) < 0) {
			printf("Ошибка перевода сокета в режим прослушивания: %s.\n", strerror(errno));
			return -1;
		}

	return s;
}

void eventLoopUDP(int serverSock, connection connList[]) {
	struct sockaddr_in clientAddr;	//структура IP-адреса клиента
	int epfd = epoll_create(EPOLL_QUEUE_LEN);			//создаем epoll-инстанс
	struct epoll_event ev;								//создаем структуру событий epoll
	struct epoll_event evList[MAX_EPOLL_EVENTS];		//создаем массив структур событий epoll
	char buffer[BUFFERSIZE];							//буфер, принимающий сообщения от клиента
	char crcServerResult[CRC32SIZE];					//буфер, в который помещается результат вычисления CRC-32
	size_t clientAddrSize = sizeof(clientAddr);

	memset(&clientAddr, 0, sizeof(clientAddr));
	memset(&buffer, 0, sizeof(buffer));
	memset(&crcServerResult, 0, sizeof(crcServerResult));

	ev.events = EPOLLIN;								//указываем маску интересующих нас событий
	ev.data.fd = serverSock;							//указываем файловый дескриптор для мониторинга событий
	epoll_ctl(epfd, EPOLL_CTL_ADD, serverSock, &ev);	//начинаем мониторинг с заданными параметрами
	//цикл мониторинга событий epoll
	while (1) {
		int i, j;											//счетчик цикла проверки готовых файловых дескрипторов
		int n = 0;										//переменная хранения результата функции read()
		char clientName[32];							//буфер для хранения хостнейма подключившегося клиента
		memset(&clientName, 0, sizeof(clientName));
		//ожидаем событий, в nfds будет помещено число готовых файловых дескрипторов
		int nfds = epoll_wait(epfd, evList, MAX_EPOLL_EVENTS, EPOLL_RUN_TIMEOUT);
		//цикл проверки готовых файловых дескрипторов
		for(i = 0; i < nfds; i++) {
			//проверяем, на каком дескрипторе произошло событие
			if(evList[i].events & EPOLLIN) {
				//если мы попали сюда, значит входящие данные от клиента ожидают своей обработки
				int k;
				//выясняем, от какого именно клиента пришли данные
				for (k = 0; k < NUM_OF_CONNECTIONS; k++) {
					if (evList[i].data.fd == connList[k].clientSockFD)
						//запоминаем номер элемента массива событий, соответствующий сокету, на котором произошло событие
						n = k;
				}

				while (1) {
					//fd_set_blocking(evList[i].data.fd, 0);
					//читаем полученные данные из сокета
					int res = recvfrom(serverSock, buffer, sizeof(buffer), 0, (struct sockaddr *)&clientAddr, &clientAddrSize);
					/*if (res == -1)
						break;*/
					//else
						if (res == 0) {
						//похоже, что клиент отключился
						printf("%s (%s) прервал соединение.\n", connList[n].clientNickName, connList[n].clientHostName);
						close(evList[i].data.fd);						//исключаем связанный с ним дескриптор из epoll-инстанс
						memset(&connList[n], 0, sizeof(connList[n]));	//удаляем информацию об этом клиенте
						break;
					}

					int done = 0;
					for(j = 0; j < NUM_OF_CONNECTIONS; j++) {
						if(connList[j].clientSockFD != evList[i].data.fd)
							done++;
					}

					if(done == NUM_OF_CONNECTIONS)
						for(j = 0; j < NUM_OF_CONNECTIONS; j++) {
							if(connList[j].clientHostName[0] == '\0') {
								connList[j].clientSockFD = evList[i].data.fd;
								//получаем хостнейм клиента по известному адресу
								getnameinfo((struct sockaddr *)&clientAddr, sizeof(clientAddr), clientName, sizeof(clientName), NULL, 0, 0);
								strcat(connList[j].clientHostName, clientName);			//сохраняем хостнейм клиента
								printf("Установлено соединение с %s!\n", clientName);
								memset(&clientName, 0, sizeof(clientName));
								ev.data.fd = evList[i].data.fd;							//указываем дескриптор клиента для мониторинга
								epoll_ctl(epfd, EPOLL_CTL_ADD, evList[i].data.fd, &ev);		//добавляем сокет клиента в epoll-инстанс
								break;
							}
							else
								if(i == NUM_OF_CONNECTIONS-1) {
									printf("%s\n", errMessage);
									write(evList[i].data.fd, errMessage, strlen(errMessage));	//отправляем уведомление о заполненности сервера
								}
						}

					//если от клиента получено предупреждающее сообщение о том, что будут присланы сегменты строки
					if(strncmp(buffer, segMessage, strlen(segMessage)) == 0) {
						Assembler(evList, i, buffer, serverSock, clientAddr);					//вызываем функцию сборщика сегментов
					}

					deSerializer(connList, n, buffer);					//разбираем строку по полям экземпляра массива структур

					//подсчитываем контрольную сумму текста присланного нам сообщения и помещаем ее в crcServerResult
					unsigned int crc = crcSlow((unsigned char *)connList[n].msg.msgText, strlen(connList[n].msg.msgText));
					sprintf(crcServerResult, "%X", crc);

					//сравниваем присланный клиентом CRC-32 с подсчитанным нами CRC-32
					if (strcmp(connList[n].msg.msgCRC32, crcServerResult) == 0) {
						//в случае совпадения, выводим текст сообщения на печать и отправляем его обратно клиенту
						printf("%s: %s\n", connList[n].clientNickName, connList[n].msg.msgText);
						write(evList[i].data.fd, connList[n].msg.msgText, strlen(connList[n].msg.msgText));
					}
					else {
						//если контрольные суммы не совпали, уведомляем об этом клиента
						write(evList[i].data.fd, errMessageCRC, strlen(errMessageCRC));
						printf("%s: Checksum missmatch.\n", connList[n].clientNickName);
					}

					memset(&connList[n].msg, 0, sizeof(connList[n].msg));
					memset(&buffer, 0, sizeof(buffer));
					memset(&crcServerResult, 0, sizeof(crcServerResult));
				}
			}
		}
	}
}

void eventLoopTCP(int serverSock, connection connList[]) {
	struct sockaddr_in clientAddr;	//структура IP-адреса клиента
	int clientSock;
	int epfd = epoll_create(EPOLL_QUEUE_LEN);			//создаем epoll-инстанс
	struct epoll_event ev;								//создаем структуру событий epoll
	struct epoll_event evList[MAX_EPOLL_EVENTS];		//создаем массив структур событий epoll
	char buffer[BUFFERSIZE];							//буфер, принимающий сообщения от клиента
	char crcServerResult[CRC32SIZE];					//буфер, в который помещается результат вычисления CRC-32

	memset(&clientAddr, 0, sizeof(clientAddr));
	memset(&buffer, 0, sizeof(buffer));
	memset(&crcServerResult, 0, sizeof(crcServerResult));

	ev.events = EPOLLIN;								//указываем маску интересующих нас событий
	ev.data.fd = serverSock;							//указываем файловый дескриптор для мониторинга событий
	epoll_ctl(epfd, EPOLL_CTL_ADD, serverSock, &ev);	//начинаем мониторинг с заданными параметрами
	//цикл мониторинга событий epoll
	while (1) {
		int i;											//счетчик цикла проверки готовых файловых дескрипторов
		int n = 0;										//переменная хранения результата функции read()
		char clientName[32];							//буфер для хранения хостнейма подключившегося клиента
		memset(&clientName, 0, sizeof(clientName));
		//ожидаем событий, в nfds будет помещено число готовых файловых дескрипторов
		int nfds = epoll_wait(epfd, evList, MAX_EPOLL_EVENTS, EPOLL_RUN_TIMEOUT);
		//цикл проверки готовых файловых дескрипторов
		for(i = 0; i < nfds; i++) {
			//проверяем, на каком дескрипторе произошло событие
			if (evList[i].data.fd == serverSock) {
				//цикл обработки входящих подключений
				while (1) {
					//принимаем подключение и проверяем результат
					clientSock = accept(serverSock, (struct sockaddr *) &clientAddr, sizeof(clientAddr));
					if (clientSock == -1)
						break;
					else {
						int i;
						//цикл записи данных о подключившемся хосте
						//данные будут записаны в первый из свободных элементов массива структур
						//если свободных мест нет, клиент будет уведомлен об этом
						for(i = 0; i < NUM_OF_CONNECTIONS; i++) {
							if(connList[i].clientHostName[0] == '\0') {
								connList[i].clientSockFD = clientSock;					//сохраняем сокет клиента
								//для корректной работы epoll переводим сокет клиента в неблокирующий режим
								fd_set_blocking(clientSock, 0);
								//получаем хостнейм клиента по известному адресу
								getnameinfo((struct sockaddr *)&clientAddr, sizeof(clientAddr), clientName, sizeof(clientName), NULL, 0, 0);
								strcat(connList[i].clientHostName, clientName);			//сохраняем хостнейм клиента
								printf("Установлено соединение с %s!\n", clientName);
								memset(&clientName, 0, sizeof(clientName));
								ev.data.fd = clientSock;								//указываем дескриптор клиента для мониторинга
								epoll_ctl(epfd, EPOLL_CTL_ADD, clientSock, &ev);		//добавляем сокет клиента в epoll-инстанс
								break;
							}
							else
								if(i == NUM_OF_CONNECTIONS-1) {
									printf("%s\n", errMessage);
									write(clientSock, errMessage, strlen(errMessage));	//отправляем уведомление о заполненности сервера
								}
						}
					}
				}
			}
			else {
				//если мы попали сюда, значит входящие данные от клиента ожидают своей обработки
				int k;
				//выясняем, от какого именно клиента пришли данные
				for (k = 0; k < NUM_OF_CONNECTIONS; k++) {
					if (evList[i].data.fd == connList[k].clientSockFD)
						//запоминаем номер элемента массива событий, соответствующий сокету, на котором произошло событие
						n = k;
				}
				while (1) {
					//читаем полученные данные из сокета
					int res = read(evList[i].data.fd, buffer, sizeof(buffer));
					if (res == -1)
						break;
					else if (res == 0) {
						//похоже, что клиент отключился
						printf("%s (%s) прервал соединение.\n", connList[n].clientNickName, connList[n].clientHostName);
						close(evList[i].data.fd);						//исключаем связанный с ним дескриптор из epoll-инстанс
						memset(&connList[n], 0, sizeof(connList[n]));	//удаляем информацию об этом клиенте
						break;
					}

					deSerializer(connList, n, buffer);					//разбираем строку по полям экземпляра массива структур

					//подсчитываем контрольную сумму текста присланного нам сообщения и помещаем ее в crcServerResult
					unsigned int crc = crcSlow((unsigned char *)connList[n].msg.msgText, strlen(connList[n].msg.msgText));
					sprintf(crcServerResult, "%X", crc);

					//сравниваем присланный клиентом CRC-32 с подсчитанным нами CRC-32
					if (strcmp(connList[n].msg.msgCRC32, crcServerResult) == 0) {
						//в случае совпадения, выводим текст сообщения на печать и отправляем его обратно клиенту
						printf("%s: %s\n", connList[n].clientNickName, connList[n].msg.msgText);
						write(evList[i].data.fd, connList[n].msg.msgText, strlen(connList[n].msg.msgText));
					}
					else {
						//если контрольные суммы не совпали, уведомляем об этом клиента
						write(evList[i].data.fd, errMessageCRC, strlen(errMessageCRC));
						printf("%s: Checksum missmatch.\n", connList[n].clientHostName);
					}

					memset(&connList[n].msg, 0, sizeof(connList[n].msg));
					memset(&buffer, 0, sizeof(buffer));
					memset(&crcServerResult, 0, sizeof(crcServerResult));
				}
			}
		}
	}
}

//реализация функции разбора подстрок строки по полям структуры
//аргументы:
//connList - массив структур, описывающие соединения с клиентами
//текущий элемент массива структур
//буфер, в котором находится строка сообщения
int deSerializer(connection *connList, int currentConn, char *buffer) {
	int i = 1;			//полезное сообщение начинается не с 0, а с 1 символа полученной строки
	int j = 0;
	int divider = 0;	//счетчик достигнутых разделителей полей структуры
	//проверяем наличие признака начала сообщения
	if (buffer[0] == 63) {
		//работаем со строкой до тех пор, пока не будет достигнут символ признака конца сообщения
		while(buffer[i] != 33) {
			//проверяем значение счетчика достигнутых разделителей полей структуры
			switch(divider) {
			//если 0, значит символы нужно записывать в поле имени протокола
			case 0:
				while(buffer[i] != 124) {
					connList[currentConn].protoName[j] = buffer[i];
					i++;
					j++;
				}
				divider++;	//достигнут очередной разделитель
				i++;		//переходим к следующему символу
				break;
			//если 1, значит символы нужно записывать в поле версии протокола
			case 1:
				j = 0;
				while(buffer[i] != 124) {
					connList[currentConn].protoVersion[j] = buffer[i];
					i++;
					j++;
				}
				divider++;
				i++;
				break;
			//если 2, значит символы нужно записывать в поле CRC-32
			case 2:
				j = 0;
				while(buffer[i] != 124) {
					connList[currentConn].msg.msgCRC32[j] = buffer[i];
					i++;
					j++;
				}
				divider++;	//достигнут очередной разделитель
				i++;		//переходим к следующему символу
				break;
			//если 3, значит символы нужно записывать в поле текста сообщения
			case 3:
				j = 0;
				while(buffer[i] != 124) {
					connList[currentConn].msg.msgText[j] = buffer[i];
					i++;
					j++;
				}
				divider++;	//достигнут очередной разделитель
				i++;		//переходим к следующему символу
				break;
			//если 4, значит символы нужно записывать в поле длины сообщения
			case 4:
				j = 0;
				while(buffer[i] != 124) {
					connList[currentConn].msg.msgLength[j] = buffer[i];
					i++;
					j++;
				}
				divider++;	//достигнут очередной разделитель
				i++;		//переходим к следующему символу
				break;
			//если 5, значит символы нужно записывать в поле никнейма клиента
			case 5:
				j = 0;
				while(buffer[i] != 124) {
					connList[currentConn].clientNickName[j] = buffer[i];
					i++;
					j++;
				}
				divider++;	//достигнут очередной разделитель
				i++;		//переходим к следующему символу
				break;
			//если 6, значит символы нужно записывать в поле имени сервиса
			case 6:
				j = 0;
				//читаем до достижения признака конца сообщения, т.к. это поле - последнее
				while(buffer[i] != 33) {
					connList[currentConn].serviceName[j] = buffer[i];
					i++;
					j++;
				}
				break;
			}
		}
		return 0;
	}
	else {
		printf("Некорректное сообщение.\n");
		return -1;
	}
}

//реализация функции сбора сегментов строки в единую строку
//аргументы:
//evlist[] - массив структур, описывающих события epoll
//currItem - текущий элемент этого массива
//buffer - буфер, в который будет записана собранная строка
void Assembler(struct epoll_event evList[], int currItem, char *buffer, int serverSock, struct sockaddr_in clientAddr) {
	int i, j;						//счетчики циклов
	size_t clAddrSize = sizeof(clientAddr);

	char strSegNum[5];				//число сегментов в строковом формате
	char accumBuffer[BUFFERSIZE];	//накопительный буфер
	char tempBuffer[MTU+1];			//временный буфер

	//инициализируем нулями строки, которые будем использовать далее
	memset(&strSegNum, 0, sizeof(strSegNum));
	memset(&accumBuffer, 0, sizeof(accumBuffer));
	memset(&tempBuffer, 0, sizeof(tempBuffer));

	//считываем число сегментов в строковом формате из конца полученного предупреждающего сообщения
	for(i = 0, j = strlen(segMessage); j < strlen(buffer); i++, j++)
		strSegNum[i] = buffer[j];

	//конвертируем число сегментов из строкового формата в целочисленный
	int a = atoi(strSegNum);

	//устанавливаем блокирующий режим для корректного обмена данными
	fd_set_blocking(evList[currItem].data.fd, 1);

	//отправляем клиенту подтверждение того, что нами получено и обработано предупреждающее сообщение
	int res = sendto(serverSock, ackMessage, strlen(ackMessage), 0, (struct sockaddr *)&clientAddr, sizeof(clientAddr));

	//цикл чтения присылаемых нам сегментов и записи их в накопительный буфер
	for(j = 0; j < a; j++) {
		//читаем очередной сегмент
		res = recvfrom(evList[currItem].data.fd, tempBuffer, sizeof(tempBuffer), 0, (struct sockaddr *)&clientAddr, &clAddrSize);
		if(res > 0) {
			//записываем его в накопительный буфер
			strncat(accumBuffer, tempBuffer, strlen(tempBuffer));
			memset(&tempBuffer, 0, sizeof(tempBuffer));
			//отправляем клиенту подтверждение, запускающее пересылку следующего сегмента
			sendto(evList[currItem].data.fd, ackMessage, strlen(ackMessage), 0, (struct sockaddr *)&clientAddr, sizeof(clientAddr));
		}
	}

	//возвращаем сокет в неблокирующий режим
	fd_set_blocking(evList[currItem].data.fd, 0);

	//записываем в целевой буфер содержимое накопительного буфера
	snprintf(buffer, strlen(accumBuffer) + 1, "%s", accumBuffer);
}

//реализация функции установки блокирующего/неблокирующего режима работы с файловым дескриптором
//аргументы:
//fd - файловый дескриптор сокета
//blocking - режим работы (0 - неблокирующий, 1 - блокирующий)
int fd_set_blocking(int fd, int blocking) {
    //сохраняем текущие флаги
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1)
        return 0;

    //устанавливаем режим работы
    if (blocking)
        flags &= ~O_NONBLOCK;
    else
        flags |= O_NONBLOCK;
    return fcntl(fd, F_SETFL, flags) != -1;
}
