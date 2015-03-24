#ifndef PROTOCOL_H_
#define PROTOCOL_H_

#define MTU 15					//максимальный размер передаваемых данных

#define PROTO_NAME "VDM_test"	//имя протокола
#define PROTO_VER "0.1"			//версия прокола

#define NUM_OF_CONNECTIONS 10	//число активных соединений
#define NUM_OF_SEGMENTS 100		//максимальное число сегментов
#define BUFFERSIZE 2048			//длина буфера
#define MSGSIZE 1536			//длина сообщения
#define CRC32SIZE 10			//длина контрольной суммы
#define NICK_SIZE 17			//длина никнейма
#define SERVICE_SIZE 10			//длина имени сервиса

const char segMessage[] = "SEG_MSGS_WILL_COME:";	//формат предупреждающего сообщения
const char ackMessage[] = "ACK";					//формат сообщения-подтверждения

const char firstService[] = "A";	//имя первого сервиса
const char secondService[] = "B";	//имя второго сервиса

//структура, описывающая сообщение и его параметры
typedef struct {
	char msgCRC32[CRC32SIZE];			//контрольная сумма
	char msgText[MSGSIZE];				//текст сообщения
	char msgLength[6];					//длина сообщения
} message;

//структура, описывающее соединение и его параметры
typedef struct {
	char protoName[10];					//имя протокола
	char protoVersion[5];				//версия протокола
	int clientSockFD;					//файловый дескриптор клиентского сокета
	char clientHostName[17];			//хостнейм клиента
	char clientNickName[NICK_SIZE];		//ник пользователя
	char serviceName[SERVICE_SIZE];		//имя сервиса
	message msg;						//сообщение и его параметры
} connection;


#endif /* PROTOCOL_H_ */
