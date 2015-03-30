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
#include <time.h>
#include "crc.h"
#include "protocol.h"

extern errno;

#define EPOLL_QUEUE_LEN 100		//длина очереди событий epoll
#define MAX_EPOLL_EVENTS 100	//максимальное число событий epoll за одну итерацию
#define EPOLL_RUN_TIMEOUT 5	//таймаут ожидания событий (-1 соответствует ожиданию до первого наступившего события)

//прототип функции создания и связывания сокета
int listener(const char *port, const char *transport, const char *qlen);

//прототип функции разбора подстрок строки по полям структуры
void deSerializer(connection *connList, int currentConn, char *buffer);

//прототип функции сбора сегментов строки в единую строку
void Assembler(struct epoll_event evList[], int currItem, char *buffer);

//прототип функции установки блокирующего/неблокирующего режима работы с файловым дескриптором
int fd_set_blocking(int fd, int blocking);

void integrityChecker(connection *connList, short currentConn, char *buffer);

void Accumulator(connection *connList, short currentConn, char *buffer);

int main(int argc, char *argv[]) {
	int serverSock, clientSock;							//дескрипторы сокетов сервера и клиента
	struct sockaddr_in clientAddr;						//структура IP-адреса клиента
	connection connList[NUM_OF_CONNECTIONS];			//массив структур с данными о клиентах и соединений с ними
	socklen_t clientAddrSize = sizeof(clientAddr);		//размер структуры адреса клиента

	time_t timeout;

	//строковая константа, которую сервер отправляет клиенту, для которого не нашлось места для подключения
	char errMessage[] = "Server: No more place for new clients.\n";
	//строковая константа, которую сервер отправляет клиенту в случае несоответствия контрольных сумм
	char CRCerrMessage[] = "Server: Checksum missmatch.\n";

	char buffer[BUFFERSIZE];							//буфер, принимающий сообщения от клиента
	char crcServerResult[CRC32SIZE];					//буфер, в который помещается результат вычисления CRC-32
	char crcMessage[BUFFERSIZE];

	//инициализируем используемые строки и структуры нулями
	memset(&connList, 0, sizeof(connList));
	memset(&buffer, 0, sizeof(buffer));
	memset(&crcServerResult, 0, sizeof(crcServerResult));
	memset(&crcMessage, 0, sizeof(crcMessage));

	//проверяем, получено ли необходимое количество аргументов
	if (argc == 4) {
		//создаем сокет сервера и выполняем его привязку с переводом в режим прослушивания
		serverSock = listener(argv[1], argv[2], argv[3]);
		//проверяем значение дескриптора сокета
		if (serverSock < 0)
			return -1;
		else {
			printf("Сервер вошел в режим прослушивания сокета. Ожидание подключений...\n");
			int epfd = epoll_create(EPOLL_QUEUE_LEN);			//создаем epoll-инстанс
			struct epoll_event ev;								//создаем структуру событий epoll
			struct epoll_event evList[MAX_EPOLL_EVENTS];		//создаем массив структур событий epoll
			ev.events = EPOLLIN;								//указываем маску интересующих нас событий
			ev.data.fd = serverSock;							//указываем файловый дескриптор для мониторинга событий
			epoll_ctl(epfd, EPOLL_CTL_ADD, serverSock, &ev);	//начинаем мониторинг с заданными параметрами

			//цикл мониторинга событий epoll
			while (1) {
				int i, j, k;									//счетчик цикла проверки готовых файловых дескрипторов
				int n = 0;										//переменная хранения результата функции read()
				char clientName[32];							//буфер для хранения хостнейма подключившегося клиента
				memset(&clientName, 0, sizeof(clientName));
				//ожидаем событий, в nfds будет помещено число готовых файловых дескрипторов
				int nfds = epoll_wait(epfd, evList, MAX_EPOLL_EVENTS, EPOLL_RUN_TIMEOUT);

				timeout = time(NULL);

				for(j = 0; j < NUM_OF_CONNECTIONS; j++) {
					int time1 = timeout - connList[j].timeout;
					if((time1 >= TIMEOUT) && (connList[j].clientHostName[0] != '\0') && (connList[j].firstMsgFlag == 1)) {
						printf("%s (%s) истекло время ожидания.\n", connList[j].clientNickName, connList[j].clientHostName);
						close(evList[j].data.fd);						//исключаем связанный с ним дескриптор из epoll-инстанс
						memset(&connList[j], 0, sizeof(connList[j]));	//удаляем информацию об этом клиенте
						memset(&buffer, 0, sizeof(buffer));
						break;
					}
				}

				//цикл проверки готовых файловых дескрипторов
				for(i = 0; i < nfds; i++) {
					//проверяем, на каком дескрипторе произошло событие
					if (evList[i].data.fd == serverSock) {
						//цикл обработки входящих подключений
						while (1) {
							//принимаем подключение и проверяем результат
							clientSock = accept(serverSock, (struct sockaddr *) &clientAddr, &clientAddrSize);
							if (clientSock == -1)
								break;
							else {
								//цикл записи данных о подключившемся хосте
								//данные будут записаны в первый из свободных элементов массива структур
								//если свободных мест нет, клиент будет уведомлен об этом
								for(j = 0; j < NUM_OF_CONNECTIONS; j++) {								//FUCK
									if(connList[j].clientHostName[0] == '\0') {
										connList[j].clientSockFD = clientSock;
										//для корректной работы epoll переводим сокет клиента в неблокирующий режим
										fd_set_blocking(clientSock, 0);
										//получаем хостнейм клиента по известному адресу
										getnameinfo((struct sockaddr *)&clientAddr, sizeof(clientAddr), clientName, sizeof(clientName), NULL, 0, 0);
										strcat(connList[j].clientHostName, clientName);			//сохраняем дескриптор сокета клиента
										printf("Установлено соединение с %s!\n", clientName);
										memset(&clientName, 0, sizeof(clientName));
										ev.data.fd = clientSock;								//указываем дескриптор клиента для мониторинга
										epoll_ctl(epfd, EPOLL_CTL_ADD, clientSock, &ev);		//добавляем сокет клиента в epoll-инстанс
										break;
									}
									else
										if(j == NUM_OF_CONNECTIONS-1) {
											printf("%s\n", errMessage);
											write(clientSock, errMessage, strlen(errMessage));	//отправляем уведомление о заполненности сервера
										}

								}
							}
						}
					}
					else {
						//если мы попали сюда, значит входящие данные от клиента ожидают своей обработки
						//выясняем, от какого именно клиента пришли данные
						for (k = 0; k < NUM_OF_CONNECTIONS; k++) {
							if (evList[i].data.fd == connList[k].clientSockFD) {
								//запоминаем номер элемента массива событий, соответствующий сокету, на котором произошло событие
								n = k;
								if(connList[n].firstMsgFlag == 0)
									connList[n].firstMsgFlag = 1;
							}
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
								memset(&buffer, 0, sizeof(buffer));
								memset(&crcServerResult, 0, sizeof(crcServerResult));
								break;
							}

							//если от клиента получено предупреждающее сообщение о том, что будут присланы сегменты строки
							if(strncmp(buffer, segMessage, strlen(segMessage)) == 0) {
								Assembler(evList, i, buffer);					//вызываем функцию сборщика сегментов
							}

							if(connList[n].segmentationFlag == 0) {
								integrityChecker(connList, n, buffer);
								if(connList[n].segmentationFlag == 1) {
									Accumulator(connList, n, buffer);
									memset(&buffer, 0, sizeof(buffer));
									break;
								}
							}
							else {
								Accumulator(connList, n, buffer);
								if(connList[n].segmentationFlag == 1) {
									memset(&buffer, 0, sizeof(buffer));
									break;
								}
							}

							deSerializer(connList, n, buffer);					//разбираем строку по полям экземпляра массива структур

							//подсчитываем контрольную сумму текста присланного нам сообщения и помещаем ее в crcServerResult
							strncpy(crcMessage, buffer, strlen(buffer) - 8);
							unsigned int crc = crcSlow((unsigned char *)crcMessage, strlen(crcMessage));
							sprintf(crcServerResult, "%X", crc);

							//сравниваем присланный клиентом CRC-32 с подсчитанным нами CRC-32
							if (strcmp(connList[n].messageCRC32, crcServerResult) == 0) {
								connList[n].timeout = time(NULL);
								if(strcmp(connList[n].serviceName, firstService) == 0) {
									//в случае совпадения, выводим текст сообщения на печать и отправляем его обратно клиенту
									printf("%s: %s\n", connList[n].clientNickName, connList[n].messageText);
									strcat(connList[n].messageText, firstSrvResponse);
									write(evList[i].data.fd, connList[n].messageText, strlen(connList[n].messageText));
								}
								else if(strcmp(connList[n].serviceName, secondService) == 0) {
									//в случае совпадения, выводим текст сообщения на печать и отправляем его обратно клиенту
									printf("%s: %s\n", connList[n].clientNickName, connList[n].messageText);
									strcat(connList[n].messageText, secondSrvResponse);
									write(evList[i].data.fd, connList[n].messageText, strlen(connList[n].messageText));
								}
								else {
									write(evList[i].data.fd, srvErrMessage, strlen(srvErrMessage)+1);
									printf("%s (%s) прервал соединение.\n", connList[n].clientNickName, connList[n].clientHostName);
									close(evList[i].data.fd);
									memset(&connList[n], 0, sizeof(connList[n]));
									memset(&buffer, 0, sizeof(buffer));
									memset(&crcServerResult, 0, strlen(crcServerResult)+1);
									break;
								}
							}
							else {
								//если контрольные суммы не совпали, уведомляем об этом клиента
								write(evList[i].data.fd, CRCerrMessage, strlen(CRCerrMessage));
								printf("%s: Checksum missmatch.\n", connList[n].clientHostName);
							}

							memset(&connList[n].messageText, 0, sizeof(connList[n].messageText));
							memset(&connList[n].messageCRC32, 0, sizeof(connList[n].messageCRC32));
							memset(&connList[n].length, 0, sizeof(connList[n].length));
							memset(&buffer, 0, sizeof(buffer));
							memset(&crcServerResult, 0, sizeof(crcServerResult));
							memset(&crcMessage, 0, sizeof(crcMessage));
						}
					}
				}
			}
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
	int s, type, proto, q_len, optval = 1;				//дескриптор и тип сокета

	q_len = atoi(qlen);
	//обнуляем структуру адреса
	memset(&sin, 0, sizeof(sin));
	//указываем тип адреса
	sin.sin_family = AF_INET;
	//указываем в качестве адреса шаблон INADDR_ANY, т.е. все сетевые интерфейсы
	sin.sin_addr.s_addr = INADDR_ANY;
	//конвертируем номер порта из пользовательского порядка байт в сетевой
	sin.sin_port = htons((unsigned short)atoi(port));
	//используем имя протокола для определения типа сокета
	if(strcmp(transport, "udp") == 0) {
		type = SOCK_DGRAM;
		proto = IPPROTO_UDP;
	}
	else if(strcmp(transport, "tcp") == 0) {
		type = SOCK_STREAM;
		proto = IPPROTO_TCP;
	}
	else {
		printf("Неверное имя транспротного протокола: %s.\n", strerror(errno));
		return -1;
	}

	//вызываем функцию создания сокета
	s = socket(PF_INET, type, proto);

	//переводим созданный сокет в неблокирующий режим для корректной работы epoll
	fd_set_blocking(s, 0);

	//проверяем правильность создания
	if (s < 0) {
		printf("Ошибка создания сокета: %s.\n", strerror(errno));
		return -1;
	}

	setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

	//привязка сокета с проверкой результата
	if(bind(s, (struct sockaddr *)&sin, sizeof(sin)) < 0) {
		printf("Ошибка связывания сокета: %s.\n", strerror(errno));
		return -1;
	}

	//запуск прослушивания с проверкой результата
	if (listen(s, q_len) < 0) {
		printf("Ошибка перевода сокета в режим прослушивания: %s.\n", strerror(errno));
		return -1;
	}
	return s;
}

//реализация функции разбора подстрок строки по полям структуры
//аргументы:
//connList - массив структур, описывающие соединения с клиентами
//текущий элемент массива структур
//буфер, в котором находится строка сообщения
void deSerializer(connection *connList, int currentConn, char *buffer) {
	int i = 0, j = 0;
	while(buffer[i] != 124) {
		connList[currentConn].protoName[j] = buffer[i];
		i++;
		j++;
	}
	i++;
	j = 0;
	while(buffer[i] != 124) {
		connList[currentConn].protoVersion[j] = buffer[i];
		i++;
		j++;
	}
	i++;
	j = 0;
	while(buffer[i] != 124) {
		connList[currentConn].length[j] = buffer[i];
		i++;
		j++;
	}
	i++;
	j = 0;
	while(buffer[i] != 124) {
		connList[currentConn].clientNickName[j] = buffer[i];
		i++;
		j++;
	}
	i++;
	j = 0;
	while(buffer[i] != 124) {
		connList[currentConn].serviceName[j] = buffer[i];
		i++;
		j++;
	}
	i++;
	j = 0;
	while(buffer[i] != 124) {
		connList[currentConn].messageText[j] = buffer[i];
		i++;
		j++;
	}
	i++;
	j = 0;
	while(buffer[i] != 0) {
		connList[currentConn].messageCRC32[j] = buffer[i];
		i++;
		j++;
	}
}

//реализация функции сбора сегментов строки в единую строку
//аргументы:
//evlist[] - массив структур, описывающих события epoll
//currItem - текущий элемент этого массива
//buffer - буфер, в который будет записана собранная строка
void Assembler(struct epoll_event evList[], int currItem, char *buffer) {
	int i, j;						//счетчики циклов

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
	write(evList[currItem].data.fd, ackMessage, strlen(ackMessage));

	//цикл чтения присылаемых нам сегментов и записи их в накопительный буфер
	for(j = 0; j < a; j++) {
		//читаем очередной сегмент
		int res = read(evList[currItem].data.fd, tempBuffer, sizeof(tempBuffer));
		if(res > 0) {
			//записываем его в накопительный буфер
			strncat(accumBuffer, tempBuffer, strlen(tempBuffer));
			memset(&tempBuffer, 0, sizeof(tempBuffer));
			//отправляем клиенту подтверждение, запускающее пересылку следующего сегмента
			write(evList[currItem].data.fd, ackMessage, strlen(ackMessage));
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

void integrityChecker(connection *connList, short currentConn, char *buffer) {
	char tempString[10];
	memset(&tempString, 0, sizeof(tempString));

	int i, j;
	for(i = 0; i < 8; i++)
		tempString[i] = buffer[i];
	if(strncmp(tempString, PROTO_NAME, strlen(PROTO_NAME)) == 0) {
		memset(&tempString, 0, sizeof(tempString));
		for(i = 9, j = 0; i < 12; i++, j++)
			tempString[j] = buffer[i];
		if(strncmp(tempString, PROTO_VER, strlen(PROTO_VER)) == 0) {
			memset(&tempString, 0, sizeof(tempString));
			i++;
			j = 0;
			while(buffer[i] != 124) {
				connList[currentConn].length[j] = buffer[i];
				i++;
				j++;
			}
			if(strlen(buffer) < (size_t)atoi(connList[currentConn].length))
				connList[currentConn].segmentationFlag = 1;
		}
	}
}

void Accumulator(connection *connList, short currentConn, char *buffer) {
	size_t len1 = strlen(connList[currentConn].storageBuffer) + strlen(buffer);
	size_t len2 = (size_t)atoi(connList[currentConn].length);
	if((len1) == len2) {
		strcat(connList[currentConn].storageBuffer, buffer);
		sprintf(buffer, "%s", connList[currentConn].storageBuffer);
		memset(&connList[currentConn].storageBuffer, 0, sizeof(connList[currentConn].storageBuffer));
		connList[currentConn].timeout = time(NULL);
		connList[currentConn].segmentationFlag = 0;
	}
	else {
		strcat(connList[currentConn].storageBuffer, buffer);
		connList[currentConn].timeout = time(NULL);
	}
}
