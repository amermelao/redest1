#define DID   0
#define DTYPE 1
#define DSEQ 2
#define DRET 3
#define DHDR 4

#define BUF_SIZE 1400+DHDR

#define DATA 'D'
#define ACK  'A'
#define CONNECT 'C'
#define CLOSE 'E'

#define CONNECTED 1
#define FREE 2
#define CLOSED 3

#define TIMEOUT 1.0
#define INTTIMEOUT 3
#define RETRIES 40

extern int Data_debug;

int Dconnect(char *hostname, char *port);
void Dbind(void* (*f)(void *), char *port);

int Dread(int cl, char *buf, int l);
void Dwrite(int cl, char *buf, int l);
void Dclose(int cl);
