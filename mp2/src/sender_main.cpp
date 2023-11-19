#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/stat.h>
#include <signal.h>
#include <string.h>
#include <sys/time.h>
#include <cmath>
#include <deque>
#include <unordered_map>

using namespace std;

struct sockaddr_in si_other;
int s, slen;

#define BUFSIZE 1024
// set maximum timeout to 10s
#define MAX_TIMEOUT 10000
#define FIN_SEQ_NUM 7733
#define INIT_TIMEOUT 200

// data structure for packet
#define MSS 1500
struct packet {
    int seq_num;
    int ack_num;
    bool ack;
    bool fin;
    short receive_window;
    int data_size;
    char data[MSS];
};

// RTT estimation variables (ms)
float estRTT;
float devRTT;
float timeoutInterval;
// timer base (ms)
time_t timer_base;
bool timer_running;

// in number of packets
uint window_size;
// flow control variables
uint rwnd;
// congestion control variables
uint ssthresh, dupACKcnt;
const uint init_ssthresh = 64;
float cwnd;
enum congestion_ctl {
    slow_start,
    congestion_avoidance,
    fast_recovery
};
congestion_ctl congestion_ctl_state;

enum connection_state {
    LISTEN,
    ESTAB,
    TIMED_WAIT,
    CLOSED
};
connection_state curr_state = LISTEN;

long next_seq_num, send_base; 
const int init_seq_num = 0;

// mutex for accessing shared variables
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
bool send_required = false;

// argument for send_handler 
struct Arg_send {
    FILE *fp;
    unsigned long long int bytesToTransfer;
};

// a queue of packets waiting to be sent
deque<packet*> pkt_queue;
// used to map sequence number to timestamp
unordered_map<int, clock_t> timetable;

void diep(char *s) {
    perror(s);
    exit(1);
}


void start_timer() {
    timer_running = true;
    timer_base = clock();
}

// function to update estimatedRTT, devRTT and timeoutInterval based on new sampleRTT
void updateTimeout(float sampleRTT) {
    // printf(">>>>>>>>>>>>>>>>>>>>>>>>>>>>\n");
    // printf("SampleRTT: %fms\n", sampleRTT);
    // printf("TimeoutInterval: %fms\n", timeoutInterval);
    float alpha = 0.125;
    float beta = 0.25;
    estRTT = (float) ((1 - alpha) * estRTT + alpha * sampleRTT);
    devRTT = (float) ((1 - beta) * devRTT + beta * abs(sampleRTT - estRTT));
    // timeoutInterval = min(estRTT + 4 * devRTT, (float)MAX_TIMEOUT);
    timeoutInterval = estRTT + 4 * devRTT;
}

// calculate sampleRTT
float sampleRTT(clock_t timestamp) {
    return (float)(clock()-timestamp)*1000/CLOCKS_PER_SEC;
}

// update status based on new ACK, timeout events
void update_control_status(bool newACK, bool timeout) {
    // printf("state: %d\n", state);
    switch (congestion_ctl_state) {
        case slow_start:
            // printf("cwnd: %f\n", cwnd);
            // printf("ssthresh: %d\n", ssthresh);
            if (cwnd >= ssthresh) {
                congestion_ctl_state = congestion_avoidance;
            } else if (dupACKcnt >= 3) {
                ssthresh = cwnd / 2;
                cwnd = ssthresh + 3;
                congestion_ctl_state = fast_recovery;
            } else if (timeout) {
                ssthresh = cwnd / 2;
                cwnd = ssthresh;
                dupACKcnt = 0;
            } else {
            }
            break;
        case congestion_avoidance:
            if (dupACKcnt >= 3) {
                ssthresh = cwnd / 2;
                cwnd = ssthresh + 3;
                congestion_ctl_state = fast_recovery;
            } else if (newACK) {
                cwnd += 1 / cwnd;
            } else if (timeout) {
                ssthresh = cwnd / 2;
                cwnd = ssthresh;
                dupACKcnt = 0;
                congestion_ctl_state = slow_start;
            } else {
            }
            break;
        case fast_recovery:
            if (dupACKcnt >= 3) {
                cwnd += 1;
            } else if (newACK) {
                cwnd = ssthresh;
                dupACKcnt = 0;
                congestion_ctl_state = congestion_avoidance;
            } else if (timeout) {
                ssthresh = cwnd / 2;
                cwnd = ssthresh;
                dupACKcnt = 0;
                congestion_ctl_state = slow_start;
            } else {
            }
            break;
    }
    ssthresh = max(ssthresh, (uint)2);
    cwnd = max(cwnd, 1.0f);
}

// thread to handle packet sending
void *send_handler(void *arg) {
    Arg_send *input = (Arg_send *)arg;
    FILE *fp = input->fp;
    unsigned long long int bytesToTransfer = input->bytesToTransfer;
    send_base = 0;
    next_seq_num = 0;
    while (1) {
        // fill in the packet to the queue
        pthread_mutex_lock(&mutex);
        window_size = min((uint)floor(cwnd), rwnd);
        while (curr_state == ESTAB && pkt_queue.size() < BUFSIZE && next_seq_num < bytesToTransfer) {
            int bytes_to_read = min(MSS, (int)(bytesToTransfer - next_seq_num));
            // read data from file
            char *data = new char[bytes_to_read];
            int read_bytes = fread(data, 1, bytes_to_read, fp);
            if (read_bytes < 0) {
                printf("Error reading file\n");
                exit(1);
            } else if (read_bytes == 0) {
                break;
            } else {
                // printf("Read %d bytes from file\n", read_bytes);
                // create packet
                packet *pkt = new packet;
                pkt->seq_num = next_seq_num;
                pkt->ack_num = 0;
                pkt->ack = false;
                pkt->fin = false;
                pkt->receive_window = 0;
                pkt->data_size = read_bytes;
                memcpy(pkt->data, data, read_bytes);
                if (read_bytes < MSS) {
                    pkt->data[read_bytes] = '\0';
                }
                // push packet to queue
                pkt_queue.push_back(pkt);
                // update next_seq_num
                next_seq_num += read_bytes;
            }
        }

        // check if no packet to send
        if (curr_state == ESTAB && pkt_queue.size() == 0) {
            curr_state = TIMED_WAIT;
            // create fin packet
            packet *fin_pkt = new packet;
            fin_pkt->seq_num = FIN_SEQ_NUM;
            fin_pkt->ack_num = 0;
            fin_pkt->ack = false;
            fin_pkt->fin = true;
            fin_pkt->receive_window = 1;
            fin_pkt->data_size = 0;
            pkt_queue.push_back(fin_pkt);
        }

        if (!timer_running) {
            start_timer();
        }
        // send packets based on window size
        int pkt_cnt = 0;
        printf("window_size: %d\n", window_size);
        while (pkt_cnt < pkt_queue.size() && pkt_cnt < window_size) {
            // send packet
            packet *pkt = pkt_queue.at(pkt_cnt);
            timetable[pkt->seq_num] = clock();
            if (pkt->fin) {
                // printf("Send: FIN\n");
            } else {
                // printf("Send: %d\n", pkt->seq_num);
            }
            if (sendto(s, pkt, sizeof (packet), 0, (struct sockaddr *) &si_other, slen) == -1)
                diep((char *)"sendto()");
            pkt_cnt++;
            // update cwnd
            if (congestion_ctl_state == slow_start) {
                cwnd += 1;
            } else if (congestion_ctl_state == congestion_avoidance) {
                cwnd += 1 / cwnd;
            } else {
                // only send the first packet when in fast recovery
                break;
            }
        }
        send_required = false;
        while (!send_required) {
            pthread_cond_wait(&cv, &mutex);
        }
        pthread_mutex_unlock(&mutex);
    }
    fclose(fp);   
    return NULL;
}

// thread to handle packet received
void *ack_handler(void *arg) {
    int recv_len;
    packet *recv_pkt = new packet;
    while (1) {
        if (curr_state == CLOSED) {
            break;
        }
        // read received packet from socket
        if ((recv_len = recvfrom(s, recv_pkt, sizeof (packet), 0, (struct sockaddr *) &si_other, (socklen_t *) & slen)) == -1)
            diep((char *)"recvfrom()");
        else if (recv_len == 0) {
            continue;
        }
        pthread_mutex_lock(&mutex);
        if (curr_state == TIMED_WAIT && recv_pkt->ack) {
            // printf("\033[31mRecv: FIN_ACK\n\033[0m");
            // finish connection after receiving FIN_ACK
            curr_state = CLOSED;
            // pop the fin packet
            pkt_queue.pop_back();
            // // send fin ack
            // packet *fin_ack_pkt = new packet;
            // fin_ack_pkt->ack = true;
            // fin_ack_pkt->ack_num = pkt->seq_num + 1;
            // fin_ack_pkt->seq_num = pkt->seq_num;
            // fin_ack_pkt->syn = false;
            // fin_ack_pkt->fin = true;
            // fin_ack_pkt->receive_window = 1;
            // pkt_queue.push_back(fin_ack_pkt);
            // send_required = true;
            // pthread_cond_signal(&cv);
            pthread_mutex_unlock(&mutex);
            return NULL;
        } else {
            // printf("\033[31mRecv: %d\n\033[0m", recv_pkt->ack_num);
            if (recv_pkt->ack && recv_pkt->ack_num > send_base) {
                // update rwnd
                rwnd = recv_pkt->receive_window;
                // update window_size
                window_size = min((uint)floor(cwnd), rwnd);
                // update timeout
                if (timetable.find(recv_pkt->seq_num) != timetable.end()) {
                    updateTimeout(sampleRTT(timetable[recv_pkt->seq_num]));
                }
                // update dupACKcnt
                dupACKcnt = 0;
                // update send_base
                send_base = recv_pkt->ack_num;
                // update queue
                while (pkt_queue.size() > 0 && pkt_queue.at(0)->seq_num < recv_pkt->ack_num) {
                    packet *pkt = pkt_queue.at(0);
                    pkt_queue.pop_front();
                    timer_base = timetable[pkt->seq_num];
                    timetable.erase(pkt->seq_num);
                    delete pkt;
                }
                // update ssthresh, cwnd, congestion_ctl_state
                update_control_status(true, false);
                send_required = true;
                pthread_cond_signal(&cv);
            } else if (recv_pkt->ack && recv_pkt->ack_num == send_base) {
                // update rwnd
                rwnd = recv_pkt->receive_window;
                // update window_size
                window_size = min((uint)floor(cwnd), rwnd);
                // update dupACKcnt
                dupACKcnt++;
                // update ssthresh, cwnd, congestion_ctl_state
                update_control_status(false, false);
                send_required = true;
                pthread_cond_signal(&cv);
            } else {
                // ignore packet
            }
            pthread_mutex_unlock(&mutex);
        }
    }
    return NULL;
}

// thread to handle timeout
void *timeout_handler(void *arg) {
    while (1) {
        if (!timer_running) {
            continue;
        }
        float time_elapsed = (float)(clock()-timer_base)*1000/CLOCKS_PER_SEC;
        pthread_mutex_lock(&mutex);
        if (time_elapsed > timeoutInterval) {
            // handle timeout scenario
            if (curr_state == ESTAB) {
                printf("TIMEOUT: exceed %fms\n", timeoutInterval);
                // update ssthresh, cwnd, congestion_ctl_state
                update_control_status(false, true);
                if (timeoutInterval*2 < MAX_TIMEOUT) {
                    timeoutInterval *= 2;
                } else {
                    timeoutInterval = MAX_TIMEOUT;
                }
                // update timer
                timer_base = clock();
                // retransmit packets
                send_required = true;
                pthread_cond_signal(&cv);
            } else if (curr_state == TIMED_WAIT) {
                printf("FIN_ACK TIMEOUT\n");
                // update timer
                timer_base = clock();
                if (timeoutInterval*2 < MAX_TIMEOUT) {
                    timeoutInterval *= 2;
                } else {
                    curr_state = CLOSED;
                    pthread_mutex_unlock(&mutex);
                    break;
                }
                // retransmit packets
                send_required = true;
                pthread_cond_signal(&cv);
            } else if (curr_state == CLOSED) {
                pthread_mutex_unlock(&mutex);
                break;
            }
        }
        pthread_mutex_unlock(&mutex);
    }
    return NULL;
    
}

// set timeout of socket
void set_sockettimeout() {
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = MAX_TIMEOUT;
    if (setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof (tv)) < 0) {
        perror("Error");
    }
}

void reliablyTransfer(char* hostname, unsigned short int hostUDPport, char* filename, unsigned long long int bytesToTransfer) {
    //Open the file
    FILE *fp;
    fp = fopen(filename, "rb");
    if (fp == NULL) {
        printf("Could not open file to send.");
        exit(1);
    }

	/* Determine how many bytes to transfer */

    slen = sizeof (si_other);

    if ((s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) == -1)
        diep((char *)"socket");

    memset((char *) &si_other, 0, sizeof (si_other));
    si_other.sin_family = AF_INET;
    si_other.sin_port = htons(hostUDPport);
    if (inet_aton(hostname, &si_other.sin_addr) == 0) {
        fprintf(stderr, "inet_aton() failed\n");
        exit(1);
    }
    // SYNSENT
    // TODO: not inplemented yet

	// initialize variables
    congestion_ctl_state = slow_start;
    curr_state = ESTAB;
    dupACKcnt = 0;
    ssthresh = init_ssthresh;
    cwnd = 1;
    rwnd = 1;
    window_size = cwnd;
    estRTT = 1000;
    devRTT = 0;
    timeoutInterval = estRTT + 4 * devRTT;
    timer_running = false;
    pthread_mutex_init(&mutex, NULL);
    pthread_cond_init(&cv, NULL);
    // start threads as handlers for managing user data, acks, and timeouts
    pthread_t user_data_thread, ack_thread, timeout_thread;
    Arg_send arg_user_data;
    arg_user_data.fp = fp;
    arg_user_data.bytesToTransfer = bytesToTransfer;
    pthread_create(&user_data_thread, NULL, send_handler, (void *)&arg_user_data);   
    pthread_create(&timeout_thread, NULL, timeout_handler, NULL); 
    pthread_create(&ack_thread, NULL, ack_handler, NULL);

    // end when FIN_ACK received
    pthread_join(ack_thread, NULL);
    pthread_join(timeout_thread, NULL);
    pthread_cancel(user_data_thread);

    printf("Closing the socket\n");
    close(s);
    return;
}



/*
 * 
 */
int main(int argc, char** argv) {

    unsigned short int udpPort;
    unsigned long long int numBytes;

    if (argc != 5) {
        fprintf(stderr, "usage: %s receiver_hostname receiver_port filename_to_xfer bytes_to_xfer\n\n", argv[0]);
        exit(1);
    }
    udpPort = (unsigned short int) atoi(argv[2]);
    numBytes = atoll(argv[4]);



    reliablyTransfer(argv[1], udpPort, argv[3], numBytes);


    return (EXIT_SUCCESS);
}


