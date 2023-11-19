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
#include <cmath>
#include <unordered_map>

using namespace std;
#define BUFFER_SIZE 1024
// set maximum timeout to 3s (for CLOSE_WAIT state)
#define MAX_TIMEOUT 3000

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
unordered_map <int, packet*> pkt_buffer;

enum connection_state {
    LISTEN,
    ESTAB,
    CLOSE_WAIT,
    CLOSED
};
connection_state curr_state = LISTEN;

struct sockaddr_in si_me, si_other;
int s, slen;

void diep(char *s) {
    perror(s);
    exit(1);
}

void reliablyReceive(unsigned short int myUDPport, char* destinationFile) {
    
    slen = sizeof (si_other);


    if ((s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) == -1)
        diep((char *)"socket");

    memset((char *) &si_me, 0, sizeof (si_me));
    si_me.sin_family = AF_INET;
    si_me.sin_port = htons(myUDPport);
    si_me.sin_addr.s_addr = htonl(INADDR_ANY);
    printf("Now binding\n");
    if (bind(s, (struct sockaddr*) &si_me, sizeof (si_me)) == -1)
        diep((char *)"bind");


	/* Now receive data and send acknowledgements */    
    FILE *fp = fopen(destinationFile, "w");
    if (fp == NULL) {
        printf("File cannot be opened\n");
        exit(1);
    }
    curr_state = ESTAB;
    packet recv_pkt;
    // the seq_num of the packet at buffer base
    int buf_base = 0;
    int ack_base = 0;
    int recv_len;
    clock_t FIN_start_time = clock();
    while (1) {
        if ((recv_len = recvfrom(s, &recv_pkt, sizeof (recv_pkt), 0, (struct sockaddr *) &si_other, (socklen_t *) & slen)) == -1) {
            diep((char *)"recvfrom()");
        } else if (recv_len == 0)
            continue;
        else {
            // printf("\033[31mRecv: %d\n\033[0m", recv_pkt.seq_num);
            // check for finish
            if (recv_pkt.fin && curr_state == ESTAB) {
                printf("\033[31mRecv: FIN\n\033[0m");
                curr_state = CLOSE_WAIT;
                // send fin ack
                packet *fin_ack_pkt = new packet;
                fin_ack_pkt->ack = true;
                fin_ack_pkt->ack_num = recv_pkt.seq_num + 1;
                fin_ack_pkt->seq_num = recv_pkt.seq_num;
                fin_ack_pkt->fin = false;
                fin_ack_pkt->receive_window = 1;
                fin_ack_pkt->data_size = 0;
                FIN_start_time = clock();
                sendto(s, fin_ack_pkt, sizeof (packet), 0, (struct sockaddr *) &si_other, slen);
                delete fin_ack_pkt;
                break;
            }
            // check for fin timeout
            if (curr_state == CLOSE_WAIT) {
                float time_elapsed = ((float)(clock()-FIN_start_time)/(float)CLOCKS_PER_SEC)*1000;
                if (time_elapsed > MAX_TIMEOUT) {
                    printf("CLOSE_WAIT timeout\n");
                    curr_state = CLOSED;
                    break;
                }
            }
            if (curr_state == ESTAB && recv_pkt.seq_num >= buf_base) {
                // check if packet is in order
                if (recv_pkt.seq_num == buf_base) {
                    // write to file
                    fwrite(recv_pkt.data, sizeof (char), recv_pkt.data_size, fp);
                    // update buffer base
                    buf_base += recv_pkt.data_size;
                    // check if there are packets in queue that can be written
                    while (pkt_buffer.find(buf_base) != pkt_buffer.end()) {
                        // write to file
                        fwrite(pkt_buffer[buf_base]->data, sizeof (char), recv_pkt.data_size, fp);
                        // update buffer base
                        buf_base += recv_pkt.data_size;
                        delete pkt_buffer[buf_base];
                        // pop from queue
                        pkt_buffer.erase(buf_base);
                    }
                    ack_base = buf_base;
                } else {
                    // store packet in queue
                    pkt_buffer[recv_pkt.seq_num] = new packet;
                    memcpy(pkt_buffer[recv_pkt.seq_num], &recv_pkt, sizeof(packet));
                }
                packet *ack_pkt = new packet;
                ack_pkt->ack = true;   
                ack_pkt->seq_num = recv_pkt.seq_num;
                ack_pkt->fin = false;
                // ack_pkt->receive_window = max(BUFFER_SIZE - (int)pkt_buffer.size(), 1);
                ack_pkt->receive_window = 512;
                ack_pkt->ack_num = ack_base;
                ack_pkt->data_size = 0;
                sendto(s, ack_pkt, sizeof (packet), 0, (struct sockaddr *) &si_other, slen);
                printf("Send: ACK %d\n", ack_pkt->ack_num);
                delete ack_pkt;
            } else {
                // send ack for the last packet received in order
                packet *ack_pkt = new packet;
                ack_pkt->ack = true;
                ack_pkt->seq_num = recv_pkt.seq_num;
                ack_pkt->fin = false;
                // ack_pkt->receive_window = max(BUFFER_SIZE - (int)pkt_buffer.size(), 1);
                ack_pkt->receive_window = 512;
                ack_pkt->ack_num = ack_base;
                ack_pkt->data_size = 0;
                sendto(s, ack_pkt, sizeof (packet), 0, (struct sockaddr *) &si_other, slen);
                // printf("Send: ACK %d\n", ack_pkt->ack_num);
                delete ack_pkt;
            }
        }
    }

    fclose(fp);

    close(s);
	printf("%s received.", destinationFile);
    return;
}

/*
 * 
 */
int main(int argc, char** argv) {

    unsigned short int udpPort;

    if (argc != 3) {
        fprintf(stderr, "usage: %s UDP_port filename_to_write\n\n", argv[0]);
        exit(1);
    }

    udpPort = (unsigned short int) atoi(argv[1]);

    reliablyReceive(udpPort, argv[2]);
}
