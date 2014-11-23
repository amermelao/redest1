#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <pthread.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>
#ifdef __MACH__
#include <mach/clock.h>
#include <mach/mach.h>
#endif
#include <sys/types.h>
#include <sys/socket.h>
#include <signal.h>
#include <string.h>
#include <math.h>
#include "jsocket6.4.h"
#include "bufbox.h"
#include "Data-rtt.h"

#define MAX_QUEUE 100 /* buffers en boxes */

/* T2: Version con threads
 * Implementa Go Back N con threads, 
 * 256 números de secuencia (0-255)
 * WIN_SZ buffers en enviador, 1 buffer en receptor
 * WIN_SZ DEBE ser < 128
 * con RTT adaptable para hacerlo mas eficiente
 * con DCNT para aceptar ACKs retransmitidos
 * Fast Retransmit: cuando llega un tercer ACK duplicado, hago como si fuera un timeout
 */


int Data_debug = 0; /* para debugging */

static int Dsock = -1;
static pthread_mutex_t Dlock;
static pthread_cond_t  Dcond;
static pthread_t Drcvr_pid, Dsender_pid;
static unsigned char ack[DHDR] = {0, ACK, 0, 0};

static void *Dsender(void *ppp);
static void *Drcvr(void *ppp);


#define max(a, b) (((a) > (b))?(a):(b))

struct {
    BUFBOX *rbox, *wbox;
    unsigned char pending_buf[WIN_SZ][BUF_SIZE];
    int pending_sz[WIN_SZ];
    int retries[WIN_SZ];
    double timeout[WIN_SZ];
    unsigned char first_w, next_w; /* window pointers */
    unsigned char first_rw; /* David: puntero del primer paquete en ventana de recepción */
    int full_win;                  /* signals fullwin to disambiguate when first==next */
    unsigned char expected_seq, expected_ack, next_seq;
    int state;
    int id;
    double rtt, rdev, rtimeout, stime[WIN_SZ];
    int dup;            /* contador de DATA duplicados, para estadística */
    int dup_acks; 	/* contador de acks duplicados */
    int dup_ack_timeout; /* flag para anotar cuando ocurrió un fast retransmit y evitar un timeout extra */
    int acked[WIN_SZ]; /* David: marcador de paquetes enviados confirmados (ventana de envío) */
    int rcvd[WIN_SZ]; /* David: marcador de paquetes recibidos (ventana de recepción) */
} connection;

/* Funciones utilitarias */

/* retorna la hora actual */
double Now() {
    struct timespec tt;
#ifdef __MACH__ // OS X does not have clock_gettime, use clock_get_time
    clock_serv_t cclock;
    mach_timespec_t mts;
    host_get_clock_service(mach_host_self(), CALENDAR_CLOCK, &cclock);
    clock_get_time(cclock, &mts);
    mach_port_deallocate(mach_task_self(), cclock);
    tt.tv_sec = mts.tv_sec;
    tt.tv_nsec = mts.tv_nsec;
#else
    clock_gettime(CLOCK_REALTIME, &tt);
#endif

    return(tt.tv_sec+1e-9*tt.tv_nsec);
}

/* Revisa rango cíclico */
int between(int x, int min, int max) {      /* min <= x < max */
    if(min <= max) return (min <= x && x < max);
    else          return (min <= x || x < max);
}

/* debugging ventana */
void print_win() {
    int p;
    int cnt, prev;

    if(connection.first_w == connection.next_w && !connection.full_win) {
        fprintf(stderr, "empty win\n");
        return;
    }
    fprintf(stderr, "win:");
    p=connection.first_w;
    cnt=0;
    do {
        cnt++;
/* Si quieren ver TODA la ventana descomentar esta linea */
        // fprintf(stderr, " %d/%d", connection.pending_buf[p][DSEQ], connection.pending_buf[p][DCNT]);
        if(cnt != 1 && (prev+1)%MAX_SEQ != connection.pending_buf[p][DSEQ]) {
            fprintf(stderr, "OJO: Out of order!\n");
            exit(1);
        }
        prev = connection.pending_buf[p][DSEQ];
        p = (p+1)%WIN_SZ;
    } while(p != connection.next_w);
    fprintf(stderr, " %d\n", cnt);
    if(cnt > WIN_SZ) { fprintf(stderr, "OJO: WIN TOO BIG!\n"); exit(1);}
}

/* Inicializa estructura conexión */
int init_connection(int id) {
    int i;

    connection.state = CONNECTED;
    connection.wbox = create_bufbox(MAX_QUEUE); 
    connection.rbox = create_bufbox(MAX_QUEUE);
    for(i=0; i < WIN_SZ; i++) {
        connection.pending_sz[i] = -1;
	connection.acked[i] = 0; /* David: todavía no se reciben ACKs */
	connection.rcvd[i] = 0; /* David: todavía no se reciben paquetes de datos */
    }
    connection.expected_ack = 1;
    connection.next_seq = 1;
    connection.expected_seq = 0;
    connection.first_w = connection.next_w = 0;
    connection.first_rw = 0; /* David: puntero de ventana de recepción */
    connection.full_win = 0;
    connection.rtt = 1.0; /* 1 s */
    connection.rdev = 0.0;
    connection.rtimeout = connection.rtt + connection.rdev;
    connection.dup = 0;
    connection.dup_acks = 0;
    connection.dup_ack_timeout = 0;
    connection.id = id;
    return id;
}

/* borra estructura conexión */
void del_connection() {
    delete_bufbox(connection.wbox);
    delete_bufbox(connection.rbox);
    connection.state = FREE;
fprintf(stderr, "Freeing connection %d\n", connection.id);
}

/* Función que inicializa los threads necesarios: sender y rcvr */
static void Init_Dlayer(int s) {

    Dsock = s;
    if(pthread_mutex_init(&Dlock, NULL) != 0) fprintf(stderr, "mutex NO\n");
    if(pthread_cond_init(&Dcond, NULL) != 0)  fprintf(stderr, "cond NO\n");
    pthread_create(&Dsender_pid, NULL, Dsender, NULL);
    pthread_create(&Drcvr_pid, NULL, Drcvr, NULL);
}

/* timer para el timeout */
void tick() {
    return;
}

/* Función que me conecta al servidor e inicializa el mundo */
int Dconnect(char *server, char *port) {
    int s, cl, i;
    struct sigaction new, old;
    unsigned char inbuf[DHDR], outbuf[DHDR];

    if(Dsock != -1) return -1;

    s = j_socket_udp_connect(server, port);
    if(s < 0) return s;

/* inicializar conexion */
    bzero(&new, sizeof new);
    new.sa_flags = 0;
    new.sa_handler = tick;
    sigaction(SIGALRM, &new, &old);

    outbuf[DTYPE] = CONNECT;
    outbuf[DID] = 0;
    outbuf[DSEQ] = 0;
    outbuf[DCNT] = 1;
    for(i=0; i < RETRIES; i++) {
        send(s, outbuf, DHDR, 0);
	outbuf[DCNT]++;
	alarm(INTTIMEOUT);
	if(recv(s, inbuf, DHDR, 0) != DHDR) continue;
	if(Data_debug) fprintf(stderr, "recibo: %c, %d\n", inbuf[DTYPE], inbuf[DID]);
	alarm(0);
	if(inbuf[DTYPE] != ACK || inbuf[DSEQ] != 0) continue;
	cl = inbuf[DID];
	break;
    }
    sigaction(SIGALRM, &old, NULL);
    if(i == RETRIES) {
	fprintf(stderr, "no pude conectarme\n");
	return -1;
    }
fprintf(stderr, "conectado, id=%d\n", cl);
    init_connection(cl);
    connection.expected_seq = (connection.expected_seq+1)%MAX_SEQ;
    Init_Dlayer(s); /* Inicializa y crea thread rcvr */
    return cl;
}

/* Lectura */
int Dread(int cl, char *buf, int l) {
int cnt;

    if(connection.id != cl) return -1;

    cnt = getbox(connection.rbox, buf, l);
    return cnt;
}

/* escritura */
void Dwrite(int cl, char *buf, int l) {
    if(connection.id != cl || connection.state != CONNECTED) return;

    putbox(connection.wbox, buf, l);
/* el lock parece innecesario, pero se necesita:
 * nos asegura que Dsender está esperando el lock o el wait
 * y no va a perder el signal! 
 */
    pthread_mutex_lock(&Dlock);
    pthread_cond_signal(&Dcond);	/* Le aviso a sender que puse datos para él en wbox */
    pthread_mutex_unlock(&Dlock);
}
/* cierra conexión */
void Dclose(int cl) {
    if(connection.id != cl) return;

    close_bufbox(connection.wbox);
    close_bufbox(connection.rbox);
}

/*
 * Aquí está toda la inteligencia del sistema: 
 * 2 threads: receptor desde el socket y enviador al socket 
 */

/* lector del socket: todos los paquetes entrantes */
static void *Drcvr(void *ppp) { 
    int cnt;
    int cl, p;
    unsigned char inbuf[BUF_SIZE];
    int found;
    double diff;

/* No verifico que me estén inyectando datos falsos de una conexion */
    while((cnt=recv(Dsock, inbuf, BUF_SIZE, 0)) > 0) {
   	if(Data_debug)
	    fprintf(stderr, "recv: id=%d, type=%c, seq=%d, retries=%d | EXP_ACK=%d, NEXT_SEQ=%d\n", inbuf[DID], inbuf[DTYPE], inbuf[DSEQ], inbuf[DCNT], connection.expected_ack, connection.next_seq);
	if(cnt < DHDR) continue;

	cl = inbuf[DID];
	if(cl != connection.id) continue;
//printf("PEGADO EN EL RECEIVER\n");
	pthread_mutex_lock(&Dlock);
//printf("DESPEGADO EN EL RECEIVER\n"); 
	if(inbuf[DTYPE] == CLOSE) { /* muy parecido a DATA */
	    if(Data_debug) fprintf(stderr, "rcv: CLOSE: %d, seq=%d, expected=%d\n\n", cl, inbuf[DSEQ], connection.expected_seq);

	/* Envio ack en todos los casos */
	    ack[DID] = cl;
	    ack[DTYPE] = ACK;
            if(inbuf[DSEQ] == connection.expected_seq) {
                ack[DSEQ] = inbuf[DSEQ];
	        ack[DCNT] = inbuf[DCNT];
	    }
            else {
                ack[DSEQ] = (connection.expected_seq+MAX_SEQ-1)%MAX_SEQ;
	        ack[DCNT] = 0; /* Ack sintético, no considerar para RTT */
	    }

	    if(Data_debug) fprintf(stderr, "Enviando CLOSE ACK %d, seq=%d\n", ack[DID], ack[DSEQ]);

	    if(send(Dsock, ack, DHDR, 0) < 0) 
		perror("sendack");

	    if(inbuf[DSEQ] != connection.expected_seq) { /* ignoramos el close */
		pthread_mutex_unlock(&Dlock);
		continue;
	    }

            connection.expected_seq = (connection.expected_seq+1)%MAX_SEQ;
	    connection.state = CLOSED;
	    fprintf(stderr, "closing %d, dups=%d\n", cl, connection.dup);
	    Dclose(cl);
	}
/* Un Ack: trabajamos para el enviador */
	else if(inbuf[DTYPE] == ACK && connection.state != FREE
                && between(inbuf[DSEQ], connection.expected_ack, connection.next_seq)) /*(connection.expected_ack+WIN_SZ-1)%MAX_SEQ))*/ {
        /* liberar buffers entre expected_ack e inbuf[DSEQ] */

		connection.dup_acks = 0;

	    if(Data_debug)
		fprintf(stderr, "recv ACK id=%d, seq=%u, retries=%d | EXP_ACK=%d, NEXT_SEQ=%d\n", inbuf[DID], inbuf[DSEQ], inbuf[DCNT],connection.expected_ack,connection.next_seq);

            if(connection.first_w == connection.next_w && !connection.full_win) {
                fprintf(stderr, "OJO: ack aceptado con ventana vacia!\n");
                exit(1);
            }
            found = 0;
            p=connection.first_w;
            do {
                if(connection.pending_buf[p][DSEQ] == inbuf[DSEQ]) {
                    found = 1;
		    connection.acked[p] = 1; /* David: marcamos la recepción del paquete */
                    break;
                }
                p=(p+1)%WIN_SZ;
            } while(p != connection.next_w);

            if(!found) {
                fprintf(stderr, "OJO: No pudo pasar: ack aceptado con ventana vacia?\n");
                exit(1);
            }
	    // if(connection.retries[p] == 0) {}
	    if(inbuf[DCNT] == connection.retries[p]) {
	    	diff = connection.rtt-(Now()-connection.stime[p]);
	    	connection.rtt -= diff*0.125;
	    	connection.rdev = 0.75*connection.rdev + (fabs(diff))*0.25;
	    	connection.rtimeout = (connection.rtt + 4*connection.rdev)*1.1;
	    	if(connection.rtimeout < MINTIMEOUT)
		    connection.rtimeout = MINTIMEOUT;
	    	if(connection.rtimeout > MAXTIMEOUT)
		    connection.rtimeout = MAXTIMEOUT;

                if(Data_debug) fprintf(stderr, "rtt=%f, diff=%f, rdev=%f, setting timeout to: %f, retries = %d\n", (Now()-connection.stime[p]), diff, connection.rdev, connection.rtimeout, inbuf[DCNT]);
	    } else if(Data_debug) fprintf(stderr, "rtt ignored, expected_retry=%d, received=%d\n", connection.retries[p], inbuf[DCNT]);

	    if(connection.state == CLOSED &&
               connection.first_w == connection.next_w &&
               !connection.full_win) {
                /* conexion cerrada y sin buffers pendientes */

                del_connection();
            }

	    if(inbuf[DSEQ] == connection.expected_ack) /* David: hay espacio en la ventana si es que recibí el ack esperado */
                connection.full_win = 0;
	    /*else {
		connection.full_win = 1;
		p = connection.first_w;
		while(p != connection.next_w) {
		    if(connection.acked[p] == 0) {
			connection.full_win = 0;
			break;
		    }
		}
	    }*/

	    while(connection.acked[connection.first_w] && !connection.full_win && connection.first_w != connection.next_w) { /* David: ACKn no implica haber recibido ACKn-1 */
		printf("DAVID\n");
                connection.first_w = (connection.first_w + 1)%WIN_SZ;
                connection.expected_ack = (connection.expected_ack + 1)%MAX_SEQ;
            }

	    pthread_cond_signal(&Dcond);
	}
	else if(inbuf[DTYPE] == DATA && connection.state == CONNECTED) {
	    if(Data_debug) fprintf(stderr, "rcv: DATA: %d, seq=%d, expected=%d\n", inbuf[DID], inbuf[DSEQ], connection.expected_seq);
	    if(boxsz(connection.rbox) >= MAX_QUEUE) { /* No tengo espacio */
		pthread_mutex_unlock(&Dlock);
		continue;
	    }
	/* envio ack en todos los otros casos */
	    ack[DID] = cl;
	    ack[DTYPE] = ACK;
            if(between(inbuf[DSEQ], connection.expected_seq, (connection.expected_seq + WIN_SZ - 1)%MAX_SEQ)) { /* David: si seq<=LAF... */
                ack[DSEQ] = inbuf[DSEQ];
	        ack[DCNT] = inbuf[DCNT];
		if(connection.rcvd[connection.first_rw + (inbuf[DSEQ]-connection.expected_seq)] == 1) { /* David: si ya lo había recibido, entonces es un DUP */
			connection.dup++;
			if(Data_debug)
                    	    fprintf(stderr, "DUP DATA seq %d, expected %d, DUP=%d\n", inbuf[DSEQ], connection.expected_seq, connection.dup);
		}
		else { /* David: si es un paquete nuevo */
			connection.rcvd[connection.first_rw + (inbuf[DSEQ]-connection.expected_seq)] = 1; /* David: se marca en la ventana de recepción */
			memcpy(connection.pending_buf[connection.first_rw + (inbuf[DSEQ]-connection.expected_seq)], inbuf, cnt); /* David: almacenamos temporalmente paquete recibido */
		}
	    }
            else {
                ack[DSEQ] = (connection.expected_seq+MAX_SEQ-1)%MAX_SEQ;
	        ack[DCNT] = 0; /* Sintetico */
                connection.dup++; /* David: es un DUP */
                if(Data_debug)
                    fprintf(stderr, "DUP DATA seq %d, expected %d, DUP=%d\n", inbuf[DSEQ], connection.expected_seq, connection.dup);
	    }

	    if(Data_debug) fprintf(stderr, "Enviando ACK %d, seq=%d, retry=%d\n", ack[DID], ack[DSEQ], ack[DCNT]);

	    if(send(Dsock, ack, DHDR, 0) <0)
		perror("sendack");

        /* si corresponde, enviar a la cola */
            if(inbuf[DSEQ] == connection.expected_seq) {
		p = connection.first_rw;
		while(connection.rcvd[p]) { /* David: se escriben de manera ordenada los paquetes recibidos */
                	putbox(connection.rbox, (char *)connection.pending_buf[p]+DHDR, cnt-DHDR);
                	connection.expected_seq = (connection.expected_seq+1)%MAX_SEQ;
			connection.rcvd[p] = 0;
			p = (p+1)%WIN_SZ;
		}
		connection.first_rw = p;		
            }
        }
        else {
	    if(Data_debug) {
/* ver si es un ACK fuera de rango y en ese caso asumir un NACK y retransmitir
   la ventana! */
                fprintf(stderr, "descarto paquete entrante: t=%c, id=%d, s=%d\n", inbuf[DTYPE], cl, inbuf[DSEQ]);
	    }
/*
 * No debo generar DUP ACK si el ACK duplicado es <= ultimo ack recibido antes del ultimo
 * timeout (es equivalente a ese timeout): marco con dup_acks = 3 cuando hago timeout?
 */
                if(inbuf[DTYPE] == ACK && inbuf[DCNT] == 0) {
		    if(Data_debug)
                    fprintf(stderr, "expected_ack=%d, next_seq=%d\n", connection.expected_ack, connection.next_seq);
		    connection.dup_acks++;
		    if(connection.dup_acks == 3 &&  /* !empty */
		      !(connection.first_w == connection.next_w && !connection.full_win)) {
			connection.timeout[connection.first_w] = Now();
			connection.dup_ack_timeout = 1;
	    		pthread_cond_signal(&Dcond);
			if(Data_debug) fprintf(stderr, "generating DUP ACK timeout!\n");
		    }
		}
        }

	pthread_mutex_unlock(&Dlock);
    }
    fprintf(stderr, "fallo read en Drcvr()\n");
    return NULL;
}

int client_retransmit() {
    int p;

    if(connection.first_w == connection.next_w && !connection.full_win)
        return 0; /* empty window */

    p=connection.first_w;
    do {
        if(connection.timeout[p] <= Now()) {
            if(Data_debug) fprintf(stderr, "timeout pack: %d, tout=%f, now=%f | FIRST=%d\n", connection.pending_buf[p][DSEQ], connection.timeout[p], Now(), connection.first_w);
            return 1;
        }
        p=(p+1)%WIN_SZ;
    } while(p != connection.next_w);

    return 0;
}

double Dclient_timeout_or_pending_data() {
    int cl, p;
    double timeout;
/* Suponemos lock ya tomado! */

    timeout = Now()+20.0;
    if(connection.state == FREE) return timeout;

    if(boxsz(connection.wbox) != 0 && !connection.full_win)
	/* data from client */
	return Now();

    if(connection.first_w == connection.next_w && ! connection.full_win)
	return timeout; /* nothing pending: empty window */

    p=connection.first_w;
    do {
	if(connection.timeout[p] <= Now()) return Now();
	if(connection.timeout[p] < timeout) timeout = connection.timeout[p];
 	p=(p+1)%WIN_SZ;
    } while(p != connection.next_w);

    return timeout;
}

/* Thread enviador y retransmisor */
static void *Dsender(void *ppp) { 
    double timeout;
    struct timespec tt;
    int i;
    int p;
    int ret;

  
    for(;;) {
	pthread_mutex_lock(&Dlock);
        /* Esperar que pase algo */
	while((timeout=Dclient_timeout_or_pending_data()) > Now()) {
 // fprintf(stderr, "timeout=%f, now=%f\n", timeout, Now());
 // fprintf(stderr, "Al tuto %f segundos\n", timeout-Now());
	    tt.tv_sec = timeout;
 // fprintf(stderr, "Al tuto %f nanos\n", (timeout-tt.tv_sec*1.0));
	    tt.tv_nsec = (timeout-tt.tv_sec*1.0)*1000000000;
 // fprintf(stderr, "Al tuto %f segundos, %d secs, %d nanos\n", timeout-Now(), tt.tv_sec, tt.tv_nsec);
//	    printf("PEGADO EN SENDER\n");
	    ret=pthread_cond_timedwait(&Dcond, &Dlock, &tt);
 // fprintf(stderr, "volvi del tuto con %d, now=%f\n", ret, Now());
	}
//printf("DESPEGADO EN SENDER\n");
	/* Revisar: timeouts y datos entrantes */

	    if(connection.state == FREE) pthread_exit(0);
            if(client_retransmit()) { /* retransmitir toda la ventana */

            if(Data_debug) fprintf(stderr, "TIMEOUT: exp_ack=%d, next_seq=%d\n", connection.expected_ack, connection.next_seq);
	    connection.dup_acks = 3; /* para evitar dups junto con el timeout */
            p=connection.first_w;
            do {
                if(connection.retries[p]++ > RETRIES) {
                    fprintf(stderr, "%d: too many retries: %d, seq=%d\n", connection.id, connection.retries[p], connection.pending_buf[p][DSEQ]);
                    del_connection();
                    pthread_exit(0);
                }
		connection.pending_buf[p][DCNT] = connection.retries[p];
                if(Data_debug) fprintf(stderr, "Re-send DATA, cl=%d, seq=%d, retries=%d | EXP_ACK=%d, FIRST=%d, P=%d, FULL_WIN=%d\n", connection.id, connection.pending_buf[p][DSEQ], connection.pending_buf[p][DCNT],connection.expected_ack, connection.first_w, p, connection.full_win);
		//if(connection.acked[p] == 1) { /* David: si ya se recibió el ack del respectivo paquete, no retransmitirlo */
		  //  p=(p+1)%WIN_SZ;
		    //continue;
		//}
                if(send(Dsock, connection.pending_buf[p], DHDR+connection.pending_sz[p], 0) < 0) {
                    perror("sendto1"); exit(1);
                }
/* timer backoff solo una vez por ventana y solo si no es retrans por dup_ack */
		if(p == connection.first_w && !connection.dup_ack_timeout) { /* first time */
		    connection.rtimeout *= 2;
		    if(connection.rtimeout > MAXTIMEOUT)
		    	connection.rtimeout = MAXTIMEOUT;

		    if(Data_debug) fprintf(stderr, "Dup timeout=%f\n", connection.rtimeout);
		}

		connection.dup_ack_timeout = 0;
		connection.stime[p] = Now();
		connection.timeout[p] = connection.stime[p] + connection.rtimeout;

                p=(p+1)%WIN_SZ;
            } while(p != connection.next_w);
            }
            if(boxsz(connection.wbox) != 0 && !connection.full_win) {
/*
                Hay un buffer para mi para enviar
                leerlo, enviarlo, marcar esperando ACK
*/
                p = connection.next_w;
                connection.pending_sz[p] = getbox(connection.wbox, (char *)connection.pending_buf[p]+DHDR, BUF_SIZE);
                connection.pending_buf[p][DID]=connection.id;
                connection.pending_buf[p][DSEQ]=connection.next_seq;
                connection.pending_buf[p][DCNT]=1;
                connection.next_seq = (connection.next_seq+1)%MAX_SEQ;
                connection.next_w = (connection.next_w+1)%WIN_SZ;
		connection.acked[p] = 0; /* David: se marca como no confirmado */
                if(connection.next_w == connection.first_w)
                    connection.full_win = 1;

                if(connection.pending_sz[p] == -1) { /* EOF */
                if(Data_debug) fprintf(stderr, "sending EOF\n");
                   connection.state = CLOSED;
                   connection.pending_buf[p][DTYPE]=CLOSE;
                   connection.pending_sz[p] = 0;
		fprintf(stderr, "closing %d, dups=%d\n", connection.id, connection.dup);
                }
                else {
                   if(Data_debug)
                        fprintf(stderr, "sending DATA id=%d, seq=%d\n", connection.id, connection.pending_buf[p][DSEQ] );
                   connection.pending_buf[p][DTYPE]=DATA;
                }

		if(Data_debug) print_win();

                sendto(Dsock, connection.pending_buf[p], DHDR+connection.pending_sz[p], 0, NULL, 0);

		connection.stime[p] = Now();
		connection.timeout[p] = connection.stime[p] + connection.rtimeout;
		connection.retries[p] = 1;
	    }
	pthread_mutex_unlock(&Dlock);
    }
    return NULL; /* unreachable */
}

