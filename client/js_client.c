#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/select.h>

#define DEBUG 0
// TO COMPILE
// use  gcc -o js_client js_client.c -lpthread

//--------------- Header struct for message sending ---------------
struct header {
	char magic1;                    //Magic numbers, chars
	char magic2;                    // that spell initials, set below
	char opcode;                    //Operation Code, keeps c/s on same page
	char payload_len;               //Length of what i sent after header

	uint32_t token;                 //session token
	uint32_t msg_id;                //Identification number of the current message
};

const int h_size = sizeof(struct header); //A const int giving size of header

// Constants for headers
char MAGIC_1 = 0x4A;	//J
char MAGIC_2 = 0x53;	//S

// Constants for states
#define STATE_OFFLINE			0
#define STATE_LOGIN_SENT		1
#define STATE_ONLINE			2

// Constants indicating the events
// All events starting with EVENT_USER_ are generated by human user.
#define EVENT_USER_LOGIN		0
#define EVENT_USER_POST			1
#define EVENT_USER_CMD_UNK		78
#define EVENT_USER_INVALID		79
#define EVENT_USER_RESET		66
#define EVENT_USER_SUBSCRIBE		20
#define EVENT_USER_UNSUBSCRIBE		21
#define EVENT_USER_RETRIEVE		40
#define EVENT_USER_LOGOUT		34
#define EVENT_USER_SR			35

// All events starting with EVENT_NET_ are generated by receiving a msg
// from the network. We deliberately use larger numbers to help debug
#define EVENT_NET_LOGIN_SUCCESSFUL	80
#define EVENT_NET_POST_ACK		81
#define EVENT_NET_FORWARD		99
#define EVENT_NET_INVALID		255
#define EVENT_NET_LOGIN_FAILED		254
#define EVENT_NET_SUBSCRIBE_ACK		90
#define EVENT_NET_SUBSCRIBE_FAILED	91
#define EVENT_NET_UNSUB_ACK		92
#define EVENT_NET_UNSUB_FAILED		93
#define EVENT_NET_RETRIEVE_ACK		192
#define EVENT_NET_RETRIEVE_ACK_END	193
#define EVENT_NET_LOGOUT		33
#define EVENT_NET_RESET			67

// Constants indicating the opcodes
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
int parse_event_from_input_string(char *);
int parse_event_from_received_message(unsigned char);
void print_error(int, int, int, int);

int main() {
	
	char user_input[1024];

	int ret;
	int sockfd = 0;
	char send_buffer[1024];
	char recv_buffer[1024];
	struct sockaddr_in serv_addr;
	struct sockaddr_in my_addr;
	int maxfd;
	fd_set read_set;
	FD_ZERO(&read_set);

	// need one socket file descriptor
	sockfd = socket(AF_INET, SOCK_DGRAM, 0);
	if (sockfd < 0 ) {
		printf("socket() error: %s.\n",strerror(errno));
		return -1;
	}
	
	// serv_addr is servers address and port number
	// must match addr in "UDP recieve" code
	// IP is 127.0.0.1 because we assume running on same machine
	memset(&serv_addr, 0, sizeof(serv_addr));
	serv_addr.sin_family = AF_INET;
	serv_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
	serv_addr.sin_port = htons(32000);

	// The "my_addr" is the client's address and port number used
	// for receiving responses from the server.
	// Local address, not remote address
	memset(&my_addr, 0, sizeof(my_addr));
	my_addr.sin_family = AF_INET;
	my_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
	my_addr.sin_port = htons(123);//some_semi_random_port_number);

	// Bind "my_addr" to the socket for receiving messages from the server
	bind(sockfd,
		(struct sockaddr *) &my_addr,
		sizeof(my_addr));

	maxfd = sockfd + 1; //the file descripter of stdin is "0"

	int state = STATE_OFFLINE;
	int event;
	uint32_t token; // Assume token is 32bit integer

	// This is a pointer of the type "struct header" but it always ponts
	// to the first byte of the "send_buffer" ie if we dereference this
	// pointer, we get the first 12 bytes in the "send_buffer" in the format
	// of the structure, which is very convenient
	struct header *ph_send = (struct header *)send_buffer;
	struct header *ph_recv = (struct header *)recv_buffer;

	while(1) {

		// Use select to wait on keyboard input or socket receiving
		FD_SET(fileno(stdin), &read_set);
		FD_SET(sockfd, &read_set);

		select(maxfd, &read_set, NULL, NULL, NULL);

		if (FD_ISSET(fileno(stdin), &read_set)) {

			// Keyboard input event.
			// Figures out which event and processes it according to
            // Current state

			fgets(user_input, sizeof(user_input), stdin);

			// Note that in this parse function, you need to
			// check the user input and figure out what event
			// it is. Basically it will be a long sequence of if
			// (strncmp(user_input, ...) == 0)
			// and if none of the if matches, return EVENT_USER_INVALID
			event = parse_event_from_input_string(user_input);

			// You can also add a line to print the "event" to debug

//
//	EVENT USER LOGIN
//
			if (event == EVENT_USER_LOGIN) {
				if (state == STATE_OFFLINE) {
// <<<<<<<<<<<<<<<<<<<<<<<<<<<<<	<
// CAUTION no need to parse the user ID and password here, assuming correct format
// Server will parse it anyway

char *id_password = user_input + strlen("login#"); // skip the "login#"
int m = strlen(id_password);

ph_send->magic1 = MAGIC_1;
ph_send->magic2 = MAGIC_2;
ph_send->opcode = OPCODE_LOGIN;
ph_send->payload_len = m;
ph_send->token = 0;
ph_send->msg_id = 0;

memcpy(send_buffer + h_size, id_password, m);

sendto(sockfd,
	send_buffer, h_size + m, 0,
	(struct sockaddr *) &serv_addr, sizeof(serv_addr));
// Once the corresponding action finishes, transit to login_sent state
state = STATE_LOGIN_SENT;
// >>>>>>>>>>>>>>>>>>>>>>>>>>>>>	>
				}
				else {
					print_error(EVENT_USER_LOGIN,
							event,
							STATE_OFFLINE,
							state);
				}

//
//	EVENT USER POST
//

			} else if (event == EVENT_USER_POST) {
//<<<<<<<<<<<<<<<<<<<<<<	<
// Note that this is similar to the login msg.
// Minimize processing on client side. similar to sub, unsub, post, retrieve
				if (state == STATE_ONLINE){
char *text = user_input + strlen("post#"); // skip "post#"
int m = strlen(text);
#if DEBUG
printf("posting (%i): %s\n",m,text);
#endif


ph_send->magic1 = MAGIC_1;
ph_send->magic2 = MAGIC_2;
ph_send->opcode = OPCODE_POST;
ph_send->payload_len = m;
ph_send->token = token;
ph_send->msg_id = 0;

memcpy(send_buffer + h_size, text, m);

sendto(sockfd,
	send_buffer, h_size + m, 0,
	(struct sockaddr *)&serv_addr, sizeof(serv_addr));
} else {
	printf("Error, must log in first\n");
}
//>>>>>>>>>>>>>>>>>>>>>>	>

//
//	EVENT USER SUBSCRIBE
//

			} else if (event == EVENT_USER_SUBSCRIBE) {
//<<<<<<<<<<<<<<<<<<<<<<	<
char *text = user_input + strlen("subscribe#"); // skip "subscribe#"
#if DEBUG
printf("subscribe to: %s\n",text);
#endif
int m = strlen(text);

ph_send->magic1 = MAGIC_1;
ph_send->magic2 = MAGIC_2;
ph_send->opcode = OPCODE_SUBSCRIBE;
ph_send->payload_len = m;
ph_send->token = token;
ph_send->msg_id = 0;

memcpy(send_buffer + h_size, text, m);

sendto(sockfd,
	send_buffer, h_size + m, 0,
	(struct sockaddr *)&serv_addr, sizeof(serv_addr));
//>>>>>>>>>>>>>>>>>>>>>>	>
				
//
//	EVENT USER UNSUBSCRIBE
//

			} else if (event == EVENT_USER_UNSUBSCRIBE) {
//<<<<<<<<<<<<<<<<<<<<<<	<
char *text = user_input + strlen("unsubscribe#"); // skip "unsubscribe#"
#if DEBUG
printf("subscribe to: %s\n",text);
#endif
int m = strlen(text);

ph_send->magic1 = MAGIC_1;
ph_send->magic2 = MAGIC_2;
ph_send->opcode = OPCODE_UNSUBSCRIBE;
ph_send->payload_len = m;
ph_send->token = token;
ph_send->msg_id = 0;

memcpy(send_buffer + h_size, text, m);

sendto(sockfd,
	send_buffer, h_size + m, 0,
	(struct sockaddr *)&serv_addr, sizeof(serv_addr));
//>>>>>>>>>>>>>>>>>>>>>>	>

//
//	EVENT USER RESET
//

			} else if (event == EVENT_USER_RESET) {
//<<<<<<<<<<<<<<<<<<<<<<	<
// EXAMPLE RESET, user just types reset# and prints a reset message
				ph_send->magic1 = MAGIC_1;
				ph_send->magic2 = MAGIC_2;
				ph_send->opcode = OPCODE_RESET;
				ph_send->payload_len = 0;
				ph_send->token = token;
				ph_send->msg_id = 0;

				sendto(sockfd,
					send_buffer, h_size, 0,
					(struct sockaddr *)&serv_addr, sizeof(serv_addr));
				token = 0;
//>>>>>>>>>>>>>>>>>>>>>>	>

			} else if (event == EVENT_USER_RETRIEVE) {
//<<<<<<<<<<<<<<<<<<<<<<	<
// Note that this is similar to the login msg.
// Minimize processing on client side. similar to sub, unsub, post, retrieve

char *number = user_input + strlen("retrieve#"); // skip "retrieve#"
int num = atoi(number);
int m = sizeof(num);
#if DEBUG
printf("retrieving %i messages\n",num);
#endif


ph_send->magic1 = MAGIC_1;
ph_send->magic2 = MAGIC_2;
ph_send->opcode = OPCODE_RETRIEVE;
ph_send->payload_len = m;
ph_send->token = token;
ph_send->msg_id = 0;

memcpy(send_buffer + h_size, number, m);

sendto(sockfd,
	send_buffer, h_size + m, 0,
	(struct sockaddr *)&serv_addr, sizeof(serv_addr));
//>>>>>>>>>>>>>>>>>>>>>>	>			

//
//	Catch-all
//
				
			} else if (event == EVENT_USER_CMD_UNK) {
//<<<<<<<<<<<<<<<<<<<<<<	<
printf("unkown command: %s\nNothing to be done\n",user_input);
//>>>>>>>>>>>>>>>>>>>>>>	>
			} else if (event == EVENT_USER_LOGOUT){
				ph_send->magic1 = MAGIC_1;
				ph_send->magic2 = MAGIC_2;
				ph_send->opcode = OPCODE_LOGOUT;
				ph_send->payload_len = 0;
				ph_send->token = token;
				ph_send->msg_id = 0;

				sendto(sockfd,
					send_buffer, h_size, 0,
					(struct sockaddr *)&serv_addr, sizeof(serv_addr));
				token = 0;
			} else if (event == EVENT_USER_SR){
				ph_send->magic1 = MAGIC_1;
				ph_send->magic2 = MAGIC_2;
				ph_send->opcode = OPCODE_SR;
				ph_send->payload_len = 0;
				ph_send->token = token;
				ph_send->msg_id = 0;

				sendto(sockfd,
					send_buffer, h_size, 0,
					(struct sockaddr *)&serv_addr, sizeof(serv_addr));
			} else {
//<<<<<<<<<<<<<<<<<<<<<<	<
printf("How did you get here? event: %i unkown command: %s\nNothing to be done\n",event,user_input);
//>>>>>>>>>>>>>>>>>>>>>>	>
			}
			
		} //end of:	if (FD_ISSET(fileno(stdin), &read_set))
		memset(user_input,0,sizeof(user_input));
		memset(send_buffer,0,sizeof(send_buffer));
		memset(recv_buffer,0,sizeof(recv_buffer));
		fflush(stdin);
//
//--------------------N E T W O R K  E V E N T S ----------
//

		if (FD_ISSET(sockfd, &read_set)) {
			
			// Now we know there is an event from the network
			// We figure out which event and process it
            // according to current state
			
			ret = recv(sockfd, recv_buffer, sizeof(recv_buffer), 0);

			event = parse_event_from_received_message((unsigned char)ph_recv->opcode);

#if DEBUG
printf("event is: %i\n",event);
#endif

			if (event == EVENT_NET_LOGIN_SUCCESSFUL) {

				if (state == STATE_LOGIN_SENT) {
					token = ph_recv->token;
					printf("login_ack#successful\n");
					state = STATE_ONLINE;
				} else {
					// spurious msg, reset session
					// consider defining "send_reset()"
					return 0;

					state = STATE_OFFLINE;
				}

			} else if (event == EVENT_NET_LOGIN_FAILED || event == EVENT_NET_INVALID) {
				if (state == STATE_LOGIN_SENT) {
					printf("login_ack#failed\n");
					state = STATE_OFFLINE;
				} else {
					printf("error: must login first\n");
					state = STATE_OFFLINE;
				}
			} else if (event == EVENT_NET_FORWARD) {
				if (state == STATE_ONLINE) {
					// Just extract the payload and print txt
					char *text = recv_buffer + h_size;

					printf("%s\n", text);
					ph_send->magic1 = MAGIC_1;
					ph_send->magic2 = MAGIC_2;
					ph_send->opcode = OPCODE_FORWARD_ACK;
					ph_send->payload_len = 0;
					ph_send->token = token;
					ph_send->msg_id = 0;

					sendto(sockfd,
						send_buffer, h_size, 0,
						(struct sockaddr *)&serv_addr, sizeof(serv_addr));
					// no state change needed
				} else {
					print_error(EVENT_NET_FORWARD,
							event,
							STATE_ONLINE,
							state);
				}
			} else if (event == EVENT_NET_POST_ACK) {
				if (state == STATE_ONLINE) {
					printf("post_ack#successful\n");
				} else {
					print_error(EVENT_NET_POST_ACK,
							event,
							STATE_ONLINE,
							state);
				}
			} else if (event == EVENT_NET_SUBSCRIBE_ACK) {
				if (state == STATE_ONLINE) {
					printf("subscribe_ack#successful\n");
				} else {
					print_error(EVENT_NET_SUBSCRIBE_ACK,
							event,
							STATE_ONLINE,
							state);
				}
			} else if (event == EVENT_NET_SUBSCRIBE_FAILED) {
				if (state == STATE_ONLINE) {
					printf("subscribe_ack#failed\n");
					state = STATE_OFFLINE;
				} else {
					return 1;
					state = STATE_OFFLINE;
				}
			} else if (event == EVENT_NET_UNSUB_ACK) {
				if (state == STATE_ONLINE) {
					printf("unsubscribe_ack#successful\n");
					state = STATE_OFFLINE;
				} else {
					return 1;
					state = STATE_OFFLINE;
				}
			} else if (event == EVENT_NET_UNSUB_FAILED) {
				if (state == STATE_ONLINE) {
					printf("unsubscribe_ack#failed\n");
					state = STATE_OFFLINE;
				} else {
					return 1;
					state = STATE_OFFLINE;
				}
			} else if (event == EVENT_NET_RETRIEVE_ACK) {
				char *text = recv_buffer + h_size;

				printf("%s", text);
			} else if (event == EVENT_NET_RETRIEVE_ACK_END) {
			} else if (event == EVENT_NET_LOGOUT) {
				printf("logout#successfull\n");
			} else if (event == EVENT_NET_RESET){
				state = STATE_OFFLINE;
				token = 0;
				printf("<server reset>\n");
			}
		//} else if (event == ___) { //If you make other events, process them here
		} //end of:	if (FD_ISSET(sockfd, &read_set))
		
	// finished processing pending event, go back to beginning of loop and wait
	} //end		while(1)
} //end 	main()

int parse_event_from_input_string(char *input){

	char* pPos = strchr(input,'#');
	if (pPos == NULL){
		return EVENT_USER_CMD_UNK;
	}
	char *delimiter = strchr(input, '#');
	*delimiter = 0; //add a null terminator
	// note that this null term can break the user ID
	// and the passsword without allocating other buffers

	if (strcmp(input,"login") == 0) {
		return EVENT_USER_LOGIN;
	}
	if (strcmp(input,"post") == 0) {
		return EVENT_USER_POST;
	}
	if (strcmp(input,"subscribe") == 0){
		return EVENT_USER_SUBSCRIBE;
	}
	if (strcmp(input,"unsubscribe") == 0){
		return EVENT_USER_UNSUBSCRIBE;
	}
	if (strcmp(input,"reset") == 0) {
		return EVENT_USER_RESET;
	}
	if (strcmp(input,"retrieve") == 0) {
		return EVENT_USER_RETRIEVE;
	}
	if (strcmp(input,"logout") == 0) {
		return EVENT_USER_LOGOUT;
	}
	if (strcmp(input,"spurres") == 0) {
		return EVENT_USER_SR;
	}
	return EVENT_USER_CMD_UNK;
}

int parse_event_from_received_message(unsigned char recv){
	if (recv == OPCODE_SUCCESSFUL_LOGIN_ACK){
		return EVENT_NET_LOGIN_SUCCESSFUL;
	}
	if (recv == OPCODE_POST_ACK) {
		return EVENT_NET_POST_ACK;
	}
	if (recv == OPCODE_SUCCESSFUL_SUBSCRIBE_ACK){
		return EVENT_NET_SUBSCRIBE_ACK;
	}
	if (recv == OPCODE_FAILED_SUBSCRIBE_ACK){
		return EVENT_NET_SUBSCRIBE_FAILED;
	}
	if (recv == OPCODE_FORWARD){
		return EVENT_NET_FORWARD;
	}
	if (recv == OPCODE_SUCCESSFUL_UNSUB_ACK){
		return EVENT_NET_UNSUB_ACK;
	}
	if (recv == OPCODE_FAILED_UNSUB_ACK){
		return EVENT_NET_UNSUB_FAILED;
	}
	if (recv == OPCODE_RETRIEVE_ACK){
		return EVENT_NET_RETRIEVE_ACK;
	}
	if (recv == OPCODE_END_OF_RETRIEVE_ACK){
		return EVENT_NET_RETRIEVE_ACK_END;
	}
	if (recv == OPCODE_MUST_LOGIN_FIRST_ERROR || recv == OPCODE_FAILED_LOGIN_ACK){
		return EVENT_NET_INVALID;
	}
	if (recv == OPCODE_LOGOUT_ACK){
		return EVENT_NET_LOGOUT;
	}
	if (recv == OPCODE_RESET){
		return EVENT_NET_RESET;
	}
#if DEBUG
printf("opcode received: %02x\n",recv);
printf("not equal to any listed in this function\n");
#endif
	return -99;
}

void print_error(int expected_event, int event, int expected_state, int state){
	//TODO define error printer
    //UNUSED
	printf("print error\n");
}









