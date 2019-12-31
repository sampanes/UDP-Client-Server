#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/select.h>
#include <sqlite3.h>

#include <time.h>

#define DEBUG 0
// TO COMPILE
// USE  gcc -o js_server js_server.c -lpthread -lsqlite3

//--------------- Header struct for message sending ---------------
struct header {
	char magic1;                    //Magic numbers, chars
	char magic2;                    // that spell initials, set below
	char opcode;                    //Operation Code, keeps c/s on same page
	char payload_len;               //Length of what i sent after header

	uint32_t token;                 //session token
	uint32_t msg_id;                //Identification number of the current message
};


const int h_size = sizeof(struct header);

// Constants for headers
char MAGIC_1 = 0x4A;	//J
char MAGIC_2 = 0x53;	//S

#define TIMEOUT_TIME	120
#define REFRESH_TIME	1

// These are constants indicating states
// CAUTION: these states have nothing to do with states on client
#define STATE_OFFLINE			0
#define STATE_ONLINE			1
#define STATE_MSG_FORWARD		2

// These are the events
// CAUTION: unrealted to client events
#define EVENT_NET_LOGIN			80
#define EVENT_NET_POST			81
#define EVENT_NET_INVALID		255
#define EVENT_NET_SUBSCRIBE		20
#define EVENT_NET_UNSUBSCRIBE		21
#define EVENT_NET_FORWARD_ACK		31
#define EVENT_NET_RETRIEVE			40
#define EVENT_NET_LOGOUT		33
#define EVENT_NET_RESET			66
#define EVENT_NET_SR			67

// Constants for opcodes, must agree on both sides
#define OPCODE_RESET			0x00
#define OPCODE_MUST_LOGIN_FIRST_ERROR	0xF0
#define OPCODE_LOGIN			0x10
#define OPCODE_SUCCESSFUL_LOGIN_ACK	0x80
#define OPCODE_FAILED_LOGIN_ACK		0x81
#define OPCODE_SUBSCRIBE		0x20
#define OPCODE_SUCCESSFUL_SUBSCRIBE_ACK	0x90
#define OPCODE_FAILED_SUBSCRIBE_ACK	0x91
#define OPCODE_UNSUBSCRIBE		0x21
#define OPCODE_SUCCESSFUL_UNSUB_ACK	0xA0
#define OPCODE_FAILED_UNSUB_ACK		0xA1
#define OPCODE_POST			0x30
#define OPCODE_POST_ACK			0xB0
#define OPCODE_FORWARD			0xB1
#define OPCODE_FORWARD_ACK		0x31
#define OPCODE_RETRIEVE			0x40
#define OPCODE_RETRIEVE_ACK		0xC0
#define OPCODE_END_OF_RETRIEVE_ACK	0xC1
#define OPCODE_LOGOUT			0x1F
#define OPCODE_LOGOUT_ACK		0x8F
#define OPCODE_SR				0xCC

//--------------- declare functions ---------------
int check_id_password(char*, char*);
int generate_random_token();
int event_from_datagram(unsigned char);
int whatNumberIsUser(char*);

//--------------- This data structure holds important information on a session ---------------
struct session {

	char client_id[32]; //assume client id is less than 32 chars
	struct sockaddr_in client_addr; //IP addr and port of client
					//for recv messages from server

	time_t last_time; //the last time server receives a msg from this client

	uint32_t token;  //the token of this session
	int state;	 //the state of session, 0 is offline

	char password[32]; //each user has a password
	int subbed[16];     //each user has a list of subscriptions, currently a bool array
                        //should certainly be switched to something like a linked list
                        //for scalability
};
struct posted_msg {
	char msg[1024];
	time_t timestamp;
};

int *getSubscribers(char*,struct session*);

//Hardcoded list of names and passwords, can be changed as needed (no "create account" yet)
// sessions are initially in offline state
char *names[16] = {"doggo", "kitty", "fishy", "gerbil", "tim", "frank", "bob", "seventh","foody", "anotherone", "tenth", "el", "tweezer", "tale", "for", "fif"};
char *passes[16] = {"doggoPass", "kittyPass", "fishyPass", "gerbilPass", "timPass", "frankPass", "bobPass", "seventhPass", "foodyPass", "anotheronePass", "tenthPass", "elPass", "tweezerPass", "talePass", "forPass", "fifPass"};

//--------------- SQLite callbacks ---------------
static int callback(void *count, int argc, char **argv, char **azColName){
   int *c = count;
   *c = atoi(argv[0]);
   return 0;
}
static int callback_2(void *msg, int argc, char **argv, char **azColName){
   char *c = msg;
   //Copy the current message to c
   strcpy(c,argv[1]);
   return 0;
}

int main() {
	struct posted_msg msg_list[50];
	int msgs_saved = 0;
	
   sqlite3 *db;
   char *zErrMsg = 0;
   int rc;
   char *sql;
	/* Open Database*/
   rc = sqlite3_open("posts.db", &db);
   if( rc ) {
      fprintf(stderr, "Can't open database: %s\n", sqlite3_errmsg(db));
      return(0);
   } else {
      fprintf(stderr, "Opened database successfully\n");
   }
   /* Create table if not exist*/
   sql = "CREATE TABLE IF NOT EXISTS POST_TABLE ("  \
	  "ID INT PRIMARY	KEY		NOT NULL,"\
      "PAYLOAD			TEXT	NOT NULL);";

   /* Execute SQL create table */
   rc = sqlite3_exec(db, sql, callback, 0, &zErrMsg);
   
   if( rc != SQLITE_OK ){
      fprintf(stderr, "SQL error: %s\n", zErrMsg);
      sqlite3_free(zErrMsg);
   } else {
      fprintf(stdout, "Post Table successful\n");
   }
   /* get msgs_saved */
   rc = sqlite3_exec(db, "select count(*) from POST_TABLE", callback, &msgs_saved, &zErrMsg);
   if( rc != SQLITE_OK ){
      fprintf(stderr, "SQL error: %s\n", zErrMsg);
      sqlite3_free(zErrMsg);
   } else {
      printf("Posts in database: %i\n",msgs_saved);
   }
	
	srand((unsigned int)time(NULL));
	int ret;
	int sockfd;
	struct sockaddr_in serv_addr, cli_addr;
	char send_buffer[1024];
	char recv_buffer[1024];
	int recv_len;
	socklen_t len;

	// Sessions are stored in array. This is not the ideal datatype
	struct session session_array[16];
	for (int ii=0;ii<16;ii++){
		memcpy(session_array[ii].client_id,names[ii],sizeof(names[ii]));
		memcpy(session_array[ii].password,passes[ii],sizeof(passes[ii]));
		for (int jj=0;jj<16;jj++){
			session_array[ii].subbed[jj] = 0;
		}
	}

	int token;

	sockfd = socket(AF_INET, SOCK_DGRAM, 0);
	if (sockfd < 0) {
		printf("socket() error: %s.\n",strerror(errno));
		return -1;
	}

	// the servaddr is the address and port number that the server will
	// keep receiving from
	memset(&serv_addr, 0, sizeof(serv_addr));
	serv_addr.sin_family = AF_INET;
	serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
	serv_addr.sin_port = htons(32000);
	
	//// FROM stackoverflow, removes blocking function
	struct timeval tv;
	tv.tv_sec = REFRESH_TIME;
	tv.tv_usec = 0;
	setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv);
	//// https://stackoverflow.com/questions/2876024/linux-is-there-a-read-or-recv-from-socket-with-timeout

	bind(sockfd,
		(struct sockaddr *) &serv_addr,
		sizeof(serv_addr));

	// address and size control structs. Makes pointer work easier
	struct header *ph_send = (struct header *)send_buffer;
	struct header *ph_recv = (struct header *)recv_buffer;

	while (1){
		//Recieve from client. Not blocking, every REFRESH_TIME it will pass thru
		len = sizeof(cli_addr);
		recv_len = recvfrom(sockfd, 		//socket file descriptor
				recv_buffer,		//recv buffer
				sizeof(recv_buffer),	//# of bytes to be revd
				0,
				(struct sockaddr *)&cli_addr,// client addr
				&len);			//len of client addr struct

		if (recv_len <= 0) {
			// Passed thru no client event... check liveliness of clients, ie scan all sessions
			// for each session, if the current time has passed TIMEOUT_TIME
			// the session expires
			time_t temp_time = time(NULL);
			
			struct session *ts;		
			for (int ii=0;ii<16;ii++){
				ts = &session_array[ii];
				if (ts->state == STATE_ONLINE){
					double seconds = difftime(temp_time,ts->last_time);
					if (seconds > TIMEOUT_TIME){
						ts->state = STATE_OFFLINE;
						ts->token = 0;
						ph_send->magic1 = MAGIC_1;
						ph_send->magic2 = MAGIC_2;
						ph_send->opcode = OPCODE_RESET;
						ph_send->payload_len = 0;
						
						cli_addr = ts->client_addr;

						sendto(sockfd, send_buffer, h_size, 0,
							(struct sockaddr *) &cli_addr, sizeof(cli_addr));
					}
				}
			}
		} else { //Recvd something, some client event

		uint32_t token = ph_recv->token;
		// extract_token_from_received_binary_msg(...);
		// This is the current session we are working with
		
		//struct session *cs = find_session_by_token(token);
		int event = event_from_datagram((unsigned char)ph_recv->opcode);
		#if DEBUG
		printf("event = %i\n",event);
		#endif
		
		struct session *cs;
		if (0 != token) { //not a login
			for (int ii = 0; ii < 16; ii ++) {
				if (session_array[ii].token == token){
					cs = &session_array[ii];
					//record the last time that this session is active
					cs->last_time = time(NULL);
					#if DEBUG
					printf("%i token=%i is equal for session %s\n",
						ii,token,session_array[ii].client_id);
					#endif
					ii=16;
				}
				#if DEBUG
				else {
					printf("session not %i\n",ii);
				}
				#endif
			}
			if (cs == NULL) {printf("NO SESSIONS?\n");}
		} else if (0 == token) {
			if (event != EVENT_NET_LOGIN){
				printf("Token is 0, not logged in\n");
				event = EVENT_NET_INVALID;
			}
		}

//
//	EVENT NET LOGIN
//

		if (event == EVENT_NET_LOGIN) {
			// for a login message, the cs should be NULL
			// and the token is 0. for other msgs, they should be valid
			// the server needs to reply a msg anyway, and this reply msg
			// contaings only the header
			ph_send->magic1 = MAGIC_1;
			ph_send->magic2 = MAGIC_2;
			ph_send->payload_len = 0;
			ph_send->msg_id = 0;
			
			char *id_password = recv_buffer + h_size;
			char* pPos = strchr(id_password,'&');
			if (pPos == NULL){
				printf("no & so no pw to process\n");
				ph_send->opcode = OPCODE_FAILED_LOGIN_ACK;
				ph_send->token = 0;
			} else {
			char *delimiter = strchr(id_password, '&');
			char *password = delimiter + 1;
			*delimiter = 0; //add a null terminator
			// note that this null term can break the user ID
			// and the passsword without allocating other buffers
			char *user_id = id_password;

			delimiter = strchr(password, '\n');
			*delimiter = 0; //add a nulll term
			// note that since we did not process it on the client side
			// and since it is always typed by a user, there must be a
			// trailing new line. we just write a null term on this
			// place to terminate the password string

			int login_success = check_id_password(user_id, password);
			if (login_success > 0) {
				//means login success
				
				ph_send->opcode = OPCODE_SUCCESSFUL_LOGIN_ACK;
				ph_send->token = generate_random_token();

				// iterate thru session array for user by ID
				for (int ii = 0; ii < 16; ii ++) {
					if (strcmp(session_array[ii].client_id,user_id) == 0){
						cs = &session_array[ii];
						#if DEBUG
						printf("found %s at %i\n",user_id,ii);
						#endif
						ii=16;
					}
					#if DEBUG
					else { printf("not %i\n",ii); }
					#endif
				}
				cs->state = STATE_ONLINE;
				cs->token = ph_send->token;
				cs->last_time = time(NULL);//??
				cs->client_addr = cli_addr;
				
			} else { //end if login success
				ph_send->opcode = OPCODE_FAILED_LOGIN_ACK;
				ph_send->token = 0;
			}
			} //end if login success and else

			sendto(sockfd, send_buffer, h_size, 0,
				(struct sockaddr *) &cli_addr, sizeof(cli_addr));
		//end if event event_login

//
//	EVENT NET POST
//

		} else if (event == EVENT_NET_POST) {
			
			//Ccheck the state of the client that sends this post msg
			//i.e. check cs->state
			#if DEBUG
			printf("cs->state = %i\n",cs->state);
			#endif
			if (cs->state != STATE_ONLINE){
				printf("ERROR with POST, %s is not online\n",cs->client_id);
			} else {
			//ELSE, we assume it is ONLINE but i dont wanna indent

			//For each target session subscribed to this publisher
			int *subscribers;
			subscribers = getSubscribers(cs->client_id, session_array);

			char *text = recv_buffer + h_size;
			char *payload = send_buffer + h_size;
			
			// This formatting the "<client_a>some_text"
			// in the payload of the fwrd msg, hence, client does
			// not need to format it. id client can just print it
			snprintf(payload,
				sizeof(send_buffer) - h_size,//ph_recv->payload_len,//
				"<%s>%s", cs->client_id, text);
			
			//ADD TO LIST OF MESSAGES, but this lis no longer used
			//Instead SQLite database stores all messages in post.db
			#if DEBUG
			printf("About to save %s as a thing\n",payload);
			#endif
			//adding to SQLite database as stated above for persistence
			char sql_insert[1024];
			snprintf(sql_insert,
					1024,	//Size 1024 might need to be expanded or decided at runtime
					"INSERT INTO POST_TABLE (ID,PAYLOAD) "\
					"VALUES (%i,'%s');",msgs_saved,payload);
			//Execute SQL statement
			rc = sqlite3_exec(db, sql_insert, callback, 0, &zErrMsg);
			
			if( rc != SQLITE_OK ){
			  fprintf(stderr, "SQL error: %s\n", zErrMsg);
			  sqlite3_free(zErrMsg);
			} else {
			  fprintf(stdout, "Records created successfully\n");
			}
			//Adding to msg_list array, unnecessary
			strcpy(msg_list[msgs_saved].msg,payload);
			msg_list[msgs_saved].timestamp = time(NULL);
			msgs_saved++;
			
			#if DEBUG
			printf("vv messages saved vv\n");
			for (int ii=0;ii<msgs_saved;ii++){
				printf("%i: %s\n",ii,msg_list[ii].msg);
			}
			#endif
			//END ADD TO LIST OF MESSAGES
			int m = strlen(payload);
			#if DEBUG
			printf("subscribers=%i\n",subscribers[0]);
			printf("message:\n%s\n",payload);
			#endif

			ph_send->magic1 = MAGIC_1;
			ph_send->magic2 = MAGIC_2;
			ph_send->opcode = OPCODE_FORWARD;
			ph_send->payload_len = m;
			ph_send->msg_id = 0; // DIDNT USE Msgid here
			for (int ii=1;ii<=subscribers[0];ii++){
				
				struct session *target = &session_array[subscribers[ii]];
				#if DEBUG
				printf("%i: forwarding '%s' to %s\n",ii,payload,target->client_id);
				#endif

				// "target" is the session struct of targ client
				target->state = STATE_MSG_FORWARD;

				sendto(sockfd, send_buffer, h_size + m, 0,
					(struct sockaddr *)&target->client_addr,
					sizeof(target->client_addr));

				//end for each target session			
			}
			// Send back the post ack to this publisher
			ph_send->magic1 = MAGIC_1;
			ph_send->magic2 = MAGIC_2;
			ph_send->opcode = OPCODE_POST_ACK;
			ph_send->payload_len = 0;
			sendto(sockfd, send_buffer, h_size, 0,
				(struct sockaddr *) &cli_addr, sizeof(cli_addr));

			//end if event event net post
			}

//
//	EVENT NET SUBSCRIBE
//
		} else if (event == EVENT_NET_SUBSCRIBE) {

			//check the state of the client that sends this post msg
			//i.e. check cs->state
			#if DEBUG
			printf("cs->state = %i\n",cs->state);
			printf("STATE_ONLINE = %i\n",STATE_ONLINE);
			#endif
			if (cs->state != STATE_ONLINE){
				printf("ERROR with SUBSCRIBE, %s is not online\n",cs->client_id);
				break;
			}
			//NOW we assume it is ONLINE because i dont wanna indent

			char *text = recv_buffer + h_size;
			for (int ii=0;ii<strlen(text);ii++){
				if (text[ii] == '\n'){
					text[ii] = '\0';
				}
			}
			int m = strlen(text);
			int user_number = whatNumberIsUser(text);
			#if DEBUG
			printf("event net subscribe, client: %s\nuser: %i\n",text,user_number);
			#endif
			if (user_number < 0){
				printf("No such user: %s\n",text);
				ph_send->magic1 = MAGIC_1;
				ph_send->magic2 = MAGIC_2;
				ph_send->opcode = OPCODE_FAILED_SUBSCRIBE_ACK;
				ph_send->payload_len = 0;
			} else {
				cs->subbed[user_number] = 1;
				ph_send->magic1 = MAGIC_1;
				ph_send->magic2 = MAGIC_2;
				ph_send->opcode = OPCODE_SUCCESSFUL_SUBSCRIBE_ACK;
				ph_send->payload_len = 0;
			}
			sendto(sockfd, send_buffer, h_size, 0,
				(struct sockaddr *) &cli_addr, sizeof(cli_addr));

//
//	forward acknowledgement
//

		} else if (event == EVENT_NET_FORWARD_ACK){
			cs->state = STATE_ONLINE;
			printf("forward_ack#successful (%s)\n",cs->client_id);
//
//	EVENT NET UNSUBSCRIBE
//

		} else if (event == EVENT_NET_UNSUBSCRIBE){

			//check the state of the client that sends this post msg
			//i.e. check cs->state
            #if DEBUG
            printf("cs->state = %i\n",cs->state);
            printf("STATE_ONLINE = %i\n",STATE_ONLINE);
            #endif
			if (cs->state != STATE_ONLINE){
				printf("ERROR with SUBSCRIBE, %s is not online\n",cs->client_id);
				break;
			}
			//NOW we assume it is ONLINE because i dont wanna indent

			char *text = recv_buffer + h_size;
			for (int ii=0;ii<strlen(text);ii++){
				if (text[ii] == '\n'){
					text[ii] = '\0';
				}
			}
			int m = strlen(text);
			int user_number = whatNumberIsUser(text);
            #if DEBUG
            printf("event net subscribe, client: %s\nuser: %i\n",text,user_number);
            #endif
			if (user_number < 0){
				printf("No such user: %s\n",text);
				ph_send->magic1 = MAGIC_1;
				ph_send->magic2 = MAGIC_2;
				ph_send->opcode = OPCODE_FAILED_SUBSCRIBE_ACK;
				ph_send->payload_len = 0;
			} else {
				cs->subbed[user_number] = 0;
				ph_send->magic1 = MAGIC_1;
				ph_send->magic2 = MAGIC_2;
				ph_send->opcode = OPCODE_SUCCESSFUL_SUBSCRIBE_ACK;
				ph_send->payload_len = 0;
			}
			sendto(sockfd, send_buffer, h_size, 0,
				(struct sockaddr *) &cli_addr, sizeof(cli_addr));
		} else if (event == EVENT_NET_RETRIEVE){
			if (cs->state != STATE_ONLINE){
				printf("ERROR with RETRIEVE, %s is not online\n",cs->client_id);
				break;
			}
			char *recv_number = recv_buffer + h_size;
			int messages_to_send = atoi(recv_number);
            #if DEBUG
            printf("retrieving %i messages\n",messages_to_send);
            #endif
			if (messages_to_send > msgs_saved){
				messages_to_send = msgs_saved;
			}
			ph_send->magic1 = MAGIC_1;
			ph_send->magic2 = MAGIC_2;
			ph_send->opcode = OPCODE_RETRIEVE_ACK;
			for (int ii = messages_to_send-1;ii>=0;ii--){
				
				char *payload = send_buffer + h_size;
				//-------------------------
				char sql_2[1024];
				sprintf(sql_2,"SELECT * FROM POST_TABLE LIMIT 1 OFFSET %d",ii);
			   /* Execute SQL statement */
			   char my_msg[1023];
			   rc = sqlite3_exec(db, sql_2, callback_2, my_msg, &zErrMsg);
			   
			   if( rc != SQLITE_OK ) {
				  fprintf(stderr, "SQL error: %s\n", zErrMsg);
				  sqlite3_free(zErrMsg);
			   } else {
				  fprintf(stdout, "SQL found message: %s\n",my_msg);
				}
				memcpy(payload,my_msg,sizeof(my_msg));
				//---------------------------------
				//memcpy(payload,msg_list[ii].msg,sizeof(msg_list[ii].msg));
				int m = strlen(payload);
                #if DEBUG
                printf("retrv msg %i len(%i): %s\n",ii,m,payload);
                #endif
				ph_send->payload_len = m;
				ph_send->msg_id = 0; // DIDNT USE Msgid here
				
				sendto(sockfd, send_buffer, h_size + m, 0,
				(struct sockaddr *) &cli_addr, sizeof(cli_addr));
			}
			ph_send->opcode = OPCODE_END_OF_RETRIEVE_ACK;
			
			sendto(sockfd, send_buffer, h_size, 0,
				(struct sockaddr *) &cli_addr, sizeof(cli_addr));
			
		} else if (event == EVENT_NET_INVALID){
			
			ph_send->magic1 = MAGIC_1;
			ph_send->magic2 = MAGIC_2;
			ph_send->opcode = OPCODE_MUST_LOGIN_FIRST_ERROR;
			ph_send->payload_len = 0;
			
			sendto(sockfd, send_buffer, h_size, 0,
				(struct sockaddr *) &cli_addr, sizeof(cli_addr));
		} else if (event == EVENT_NET_LOGOUT){
			cs->state = STATE_OFFLINE;
			cs->token = 0;
			ph_send->magic1 = MAGIC_1;
			ph_send->magic2 = MAGIC_2;
			ph_send->opcode = OPCODE_LOGOUT_ACK;
			ph_send->payload_len = 0;
		
			sendto(sockfd, send_buffer, h_size, 0,
				(struct sockaddr *) &cli_addr, sizeof(cli_addr));
		} else if (event == EVENT_NET_RESET){
			printf("<client reset>\n");
			cs->state = STATE_OFFLINE;
			cs->token = 0;
			ph_send->magic1 = MAGIC_1;
			ph_send->magic2 = MAGIC_2;
			ph_send->opcode = OPCODE_LOGOUT_ACK;
			ph_send->payload_len = 0;
		
			sendto(sockfd, send_buffer, h_size, 0,
				(struct sockaddr *) &cli_addr, sizeof(cli_addr));
		} else if (event == EVENT_NET_SR){
			printf("<network reset>\n");
			cs->state = STATE_OFFLINE;
			cs->token = 0;
			ph_send->magic1 = MAGIC_1;
			ph_send->magic2 = MAGIC_2;
			ph_send->opcode = OPCODE_RESET;
			ph_send->payload_len = 0;
		
			sendto(sockfd, send_buffer, h_size, 0,
				(struct sockaddr *) &cli_addr, sizeof(cli_addr));
		} //else if (event == ___) { //Note** can add, process other events}

		time_t current_time = time(NULL);

		memset(recv_buffer,0,sizeof(recv_buffer));
		memset(send_buffer,0,sizeof(send_buffer));
	} //end else recv was for sure not 0
	} //end		while(1)
	
	sqlite3_close(db);
	return 0;
} //END of main()

int check_id_password(char *user_id, char *password){
	for (int ii=0; ii<16; ii++){
		if (strcmp(names[ii],user_id) == 0){
#if DEBUG
printf("Name %s found!\n",user_id);
#endif
			if (strcmp(passes[ii],password) == 0){
#if DEBUG
printf("Name and password match :)\n");
#endif
				return 1;
			}
		}
	}
	return 0;
}

int generate_random_token(){
	int random = rand();
	return random;
}

int event_from_datagram(unsigned char opcode){
	if (opcode == OPCODE_LOGIN){
		return EVENT_NET_LOGIN;
	}
	if (opcode == OPCODE_POST){
		return EVENT_NET_POST;
	}
	if (opcode == OPCODE_SUBSCRIBE){
		return EVENT_NET_SUBSCRIBE;
	}
	if (opcode == OPCODE_UNSUBSCRIBE){
		return EVENT_NET_UNSUBSCRIBE;
	}
	if (opcode == OPCODE_FORWARD_ACK){
		return EVENT_NET_FORWARD_ACK;
	}
	if (opcode == OPCODE_RETRIEVE){
		return EVENT_NET_RETRIEVE;
	}
	if (opcode == OPCODE_LOGOUT){
		return EVENT_NET_LOGOUT;
	}
	if (opcode == OPCODE_RESET){
		return EVENT_NET_RESET;
	}
	if (opcode == OPCODE_SR){
		return EVENT_NET_SR;
	}
	return -99;
}

int *getSubscribers(char *user_id, struct session *session_array){
	static int ret[17];
	int howManySubs = 0;
	int user_number = whatNumberIsUser(user_id);
	for (int ii=0;ii<16;ii++){
		if (session_array[ii].subbed[user_number]){
			howManySubs++;
			ret[howManySubs] = ii;
#if DEBUG
printf("getSubscribers: %s is subbed, #%i\n",session_array[ii].client_id,howManySubs);
#endif
		}
	}
	ret[0] = howManySubs;
	return ret;
}

int whatNumberIsUser(char *user_id){
	for (int ii=0;ii<16;ii++){
		if (strcmp(names[ii],user_id) == 0){
			return ii;
		}
	}
	return -1;	
}





