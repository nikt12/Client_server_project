#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <errno.h>
#include <arpa/inet.h>
#include <math.h>
#include <fcntl.h>
#include "crc.h"
#include "protocol.h"

extern errno;

//прототип функции создания сокета и подключения к хосту
int connectSocket(const char *address, const char *port, const char *transport);

//прототип функции конвертирования данных структуры в строку для пересылки по сети
int Serializer(connection *connection, char *buffer);

//прототип функции сегментирования строк, превышающих по длине размер MTU
int Divider(int sockFD, char *buffer, struct sockaddr_in serverAddr);

//прототип функции установки блокирующего/неблокирующего режима работы с файловым дескриптором
int fd_set_blocking(int fd, int blocking);

int main(int argc, char *argv[]) {
	int sockFD;								//файловый дескриптор сокета клиента

	connection conn;//структура, описывающая данные клиента и параметры соединения

	struct sockaddr_in serverAddr;	//структура, содержащая информацию об адресе

	char exitpr[] = "exitpr";//строковая константа, по которой осуществляется выход из программы
	char errMessage[] = "No more place for new clients.";//сообщение, получаемое от сервера в случае, если на нем нет свободного места

	char buffer[BUFFERSIZE];			//буфер, принимающий ответ от сервера
	char tempBuffer[MSGSIZE];				//временный буфер для различных нужд

	//инициализируем используемые строки и структуры нулями
	memset(&conn, 0, sizeof(conn));
	memset(&buffer, 0, sizeof(buffer));
	memset(&tempBuffer, 0, sizeof(tempBuffer));
	memset(&serverAddr, 0, sizeof(serverAddr));

	//проверяем, получено ли необходимое количество аргументов
	if (argc == 4) {
		//вызываем функцию создания сокета и подключения к хосту
		sockFD = connectSocket(argv[1], argv[2], argv[3]);
		//проверяем результат
		if (sockFD < 0) {
			printf("Ошибка подключения к %s:%s!\n", argv[1], argv[2]);
			return 0;
		} else
			printf("Вы подключены к %s:%s!\n\n", argv[1], argv[2]);

		serverAddr.sin_family = AF_INET;
		serverAddr.sin_port = htons(atoi(argv[2]));
		inet_pton(AF_INET, argv[1], &serverAddr.sin_addr);

		//цикл получения никнейма и имени сервиса
		while (1) {
			//создаем и обнуляем временный буфер
			char cutBuffer[MSGSIZE];
			memset(&cutBuffer, 0, sizeof(cutBuffer));

			printf("Введите Ваш ник (от 4 до 15 символов, без пробелов): ");
			fgets(tempBuffer, sizeof(tempBuffer), stdin);
			//избавляемся от \n, который fgets помещает в конец строки
			strncpy(cutBuffer, tempBuffer, strlen(tempBuffer) - 1);
			memset(&tempBuffer, 0, sizeof(tempBuffer));
			//ник не должен быть короче 4 и длиннее 15 символов
			if ((strlen(cutBuffer) >= 4) && (strlen(cutBuffer) <= 15)) {
				//сохраняем ник
				strncpy(conn.clientNickName, cutBuffer, strlen(cutBuffer));
				memset(&cutBuffer, 0, sizeof(cutBuffer));
				while (1) {
					printf("Введите имя сервиса (%s или %s): ", firstService,
							secondService);
					fgets(tempBuffer, sizeof(tempBuffer), stdin);
					//избавляемся от \n, который fgets помещает в конец строки
					strncpy(cutBuffer, tempBuffer, strlen(tempBuffer) - 1);
					memset(&tempBuffer, 0, sizeof(tempBuffer));
					//убеждаемся, что клиент запросил один из двух доступных сервисов
					if ((strncmp(cutBuffer, firstService, strlen(firstService))
							== 0)
							|| (strncmp(cutBuffer, secondService,
									strlen(secondService)) == 0)) {
						//сохраняем имя сервиса
						strncpy(conn.serviceName, cutBuffer, strlen(cutBuffer));
						memset(&cutBuffer, 0, sizeof(cutBuffer));
						break;
					}
					memset(&cutBuffer, 0, sizeof(cutBuffer));
				}
				break;
			}
			memset(&cutBuffer, 0, sizeof(cutBuffer));
		}

		//цикл обмена данных с сервером
		while (1) {
			int n;
			printf("Введите текст сообщения: ");
			memset(&tempBuffer, 0, sizeof(tempBuffer));
			fgets(tempBuffer, sizeof(tempBuffer), stdin);
			//избавляемся от \n, который fgets помещает в конец строки
			strncpy(conn.msg.msgText, tempBuffer, strlen(tempBuffer) - 1);
			//проверяем, не ввел ли клиент команду завершения программы
			if (strcmp(conn.msg.msgText, exitpr) == 0) {
				printf("Клиент закрывается.\n\n");
				break;
			}

			strcpy(conn.protoName, PROTO_NAME);	//сохраняем имя нашего протокола
			strcpy(conn.protoVersion, PROTO_VER);//сохраняем версию нашего протокола
			//сохраняем контрольную сумму сообщения
			sprintf(conn.msg.msgCRC32, "%X",
					(unsigned int) crcSlow((unsigned char *) conn.msg.msgText,
							strlen(conn.msg.msgText)));

			Serializer(&conn, buffer);	//преобразуем структуру в единую строку

			//проверяем, превышает ли длина получившейся строки размер MTU
			if ((strcmp(argv[3], "udp") == 0) && (strlen(buffer) > (int) MTU)) {
				//вызов функции сегментирования строки с проверкой результата
				if (Divider(sockFD, buffer, serverAddr) > 0)
					printf("Серверу отправлено: %s\n", conn.msg.msgText);
				memset(&buffer, 0, sizeof(buffer));
			} else {
				//отправляем данные серверу и проверяем результат
				if (strcmp(argv[3], "tcp") == 0)
					n = write(sockFD, buffer, strlen(buffer));
				else
					n = sendto(sockFD, buffer, strlen(buffer), 0,
							(struct sockaddr *) &serverAddr,
							sizeof(serverAddr));
				if (n < 0)
					printf("Ошибка при записи данных в сокет: %s.\n",
							strerror(errno));
				else
					printf("Серверу отправлено: %s\n", conn.msg.msgText);
				memset(&buffer, 0, sizeof(buffer));
			}

			//к этому моменту на стороне сервера наш сокет уже сделан неблокирующим, и, чтобы дождаться ответа
			//от сервера, нужно вернуть его в блокирующий режим
			fd_set_blocking(sockFD, 1);

			//чтение данных из сокета с проверкой результата
			if (strcmp(argv[3], "tcp") == 0)
				n = read(sockFD, buffer, sizeof(buffer));
			else
				n = recvfrom(sockFD, buffer, sizeof(buffer), 0,
						(struct sockaddr *) &serverAddr, sizeof(serverAddr));
			if (n < 0)
				printf("Ошибка при чтении данных из сокета: %s.\n",
						strerror(errno));
			else
				printf("Ответ сервера: %s\n\n", buffer);

			//возвращаем сокет в блокирующий режим для корректной работы сервера
			fd_set_blocking(sockFD, 0);

			//проверяем, не прислал ли нам сервер сигнал о том, что на нем нет места для подключения
			if (strncmp(errMessage, buffer, strlen(buffer)) == 0) {
				printf("Клиент закрывается.\n\n");
				break;
			}
			//обнуляем только ту часть структуры, которая содержит данные сообщения
			memset(&conn.msg, 0, sizeof(conn.msg));
			memset(&buffer, 0, sizeof(buffer));
		}
		// закрываем файловый дескриптор сокета
		close(sockFD);
	} else
		//если введено неверное количество аргументов, выводим правильный формат запуска программы
		printf("Использование: %s address port transport\n", argv[0]);

	return 0;
}

//реализация функции создания сокета и подключения к хосту
//аргументы:
//address - адрес хоста
//port - порт хоста
//transport - имя транспортного протокола
int connectSocket(const char *address, const char *port, const char *transport) {
	int sockFD;
	int portNum;						//номер порта в целочисленном формате
	int type, proto;						//тип транспортного протокола
	struct sockaddr_in sockAddr;

	memset(&sockAddr, 0, sizeof(sockAddr));

	//используем имя протокола для определения типа сокета
	if (strcmp(transport, "udp") == 0) {
		type = SOCK_DGRAM;					//UDP
		proto = IPPROTO_UDP;
	} else if (strcmp(transport, "tcp") == 0) {
		type = SOCK_STREAM;					//TCP
		proto = IPPROTO_TCP;
	} else {
		printf("Некооректно указан транспортный протокол.\n");
		return -1;
	}

	//вызываем функцию создания сокета с проверкой результата
	sockFD = socket(PF_INET, type, proto);
	if (sockFD < 0) {
		printf("Ошибка создания сокета: %s.\n", strerror(errno));
		return -1;
	}

	portNum = atoi(port);//преобразовываем номер порта из строкового формата в целочисленный
	sockAddr.sin_port = htons(portNum);	//конвертируем номер порта из пользовательского порядка байт в сетевой
	sockAddr.sin_family = AF_INET;		//указываем тип адреса

	//конвертируем адрес в бинарный формат
	inet_pton(AF_INET, address, &sockAddr.sin_addr);

	if (type == SOCK_STREAM)
		//вызываем функцию подключения к хосту с проверкой результата
		if (connect(sockFD, (struct sockaddr *) &sockAddr, sizeof(sockAddr))
				< 0) {
			printf("Ошибка подключения к %s: %s (%s)!\n", address, port,
					strerror(errno));
			return -1;
		}

	return sockFD;
}

//реализация функции конвертирования данных структуры в строку для пересылки по сети
//аргументы:
//connection - структура данных о клиенте и параметрах соединения
//buffer - строка, в которую будет помещен результат
int Serializer(connection *connection, char *buffer) {
	strcat(buffer, "?");			//добавляем в буфер признак начала сообщения
	strcat(buffer, connection->protoName);
	strcat(buffer, "|");//здесь и далее "|" используется для разделения полей структуры
	strcat(buffer, connection->protoVersion);
	strcat(buffer, "|");
	strcat(buffer, connection->msg.msgCRC32);
	strcat(buffer, "|");
	strcat(buffer, connection->msg.msgText);
	strcat(buffer, "|");
	sprintf(connection->msg.msgLength, "%d",
			(int) strlen(connection->msg.msgText));
	strcat(buffer, connection->msg.msgLength);
	strcat(buffer, "|");
	strcat(buffer, connection->clientNickName);
	strcat(buffer, "|");
	strcat(buffer, connection->serviceName);
	strcat(buffer, "!");			//добавляем в буфер признак конца сообщения
	//проверяем корректность созданной строки
	if ((buffer[0] == 63) && (buffer[strlen(buffer)] == 33))
		return 0;
	else
		return -1;
}

//реализация функции сегментирования строки
//аргументы:
//sockFD - файловый дескриптор сокета
//buffer - исходная строка
int Divider(int sockFD, char *buffer, struct sockaddr_in serverAddr) {
	int segNum;				//число сегментов, на которые будет поделена строка
	int i, j, k;							//счетчики циклов
	int done = 0;							//счетчик отправленных сегментов
	int res;

	char synMessage[25];//сообщение, предупреждающее сервер о том, что будет отправлена последовательность сегментов
	char strSegNum[5];						//число сегментов в формате строки
	char tempBuffer[5];						//временный буфер

	char segArray[NUM_OF_SEGMENTS][MTU + 1];//массив строк-сегментов исходной строки

	//инициализируем нулями используемые строки и массив строк
	memset(&synMessage, 0, sizeof(synMessage));
	memset(&strSegNum, 0, sizeof(strSegNum));
	memset(&tempBuffer, 0, sizeof(tempBuffer));
	memset(&segArray, 0, sizeof(segArray));

	//проверяем, во сколько раз строка больше, чем MTU
	if ((strlen(buffer) % MTU) == 0)
		segNum = strlen(buffer) / MTU;
	else
		segNum = strlen(buffer) / MTU + 1;

	//формируем предупреждающее сообщение и доабвляем в его конец число сегментов
	strcat(synMessage, segMessage);
	sprintf(strSegNum, "%d", segNum);
	strncat(synMessage, strSegNum, strlen(strSegNum));

	//для корректного обмена переводим сокет в блокирующий режим
	fd_set_blocking(sockFD, 1);

	//отправляем предупреждающее сообщение
	res = sendto(sockFD, synMessage, strlen(synMessage), 0,
			(struct sockaddr *) &serverAddr, sizeof(serverAddr));

	//ждем подтверждения от сервера
	if ((recvfrom(sockFD, tempBuffer, sizeof(tempBuffer), 0,
			(struct sockaddr *) &serverAddr, sizeof(serverAddr)) > 0)
			&& (strncmp(tempBuffer, ackMessage, strlen(ackMessage)) == 0)) {
		memset(&tempBuffer, 0, sizeof(tempBuffer));
		//цикл сегментирования исходной строки
		for (j = 0; j < segNum; j++)
			for (i = (j * strlen(buffer)) / segNum, k = 0;
					i < ((j + 1) * strlen(buffer)) / segNum; i++, k++)
				segArray[j][k] = buffer[i];
		//цикл обмена данными с сервером
		for (j = 0; j < segNum; j++) {
			//отправляем очередной сегмент
			sendto(sockFD, segArray[j], strlen(segArray[j]), 0,
					(struct sockaddr *) &serverAddr, sizeof(serverAddr));
			//ждем подтверждения
			if ((recvfrom(sockFD, tempBuffer, sizeof(tempBuffer), 0,
					(struct sockaddr *) &serverAddr, sizeof(serverAddr)) > 0)
					&& (strncmp(tempBuffer, ackMessage, strlen(ackMessage)) == 0)) {
				memset(&tempBuffer, 0, sizeof(tempBuffer));
				//инкрементируем счетчик отправленных сегментов в случае успеха
				done++;
			}
		}
	}

	//возвращаем сокет в неблокирующий режим
	fd_set_blocking(sockFD, 0);

	memset(&buffer, 0, sizeof(buffer));

	//в случае, если счетчик отправленных сегментов равен предварительно рассчитанному числу сегментов, возвращаем число сегментов
	if (done == segNum)
		return segNum;
	else
		return -1;
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
