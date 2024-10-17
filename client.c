#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/time.h>
#include <poll.h>

#include "utils.h"

#define SLOW_START_STATE 0
#define CONGESTION_AVOIDANCE_STATE 1
#define FAST_RECOVERY_STATE 2
#define EFFECTIVE_PAYLOAD_CAPACITY PAYLOAD_SIZE
#define MAX_RETRANSMISSION_THRESHOLD 3
#define INITIAL_CONGESTION_WINDOW_SIZE 1
#define INITIAL_RETRANSMISSION_COUNT 0
#define ACK_TIMEOUT_MS 108
#define SLEEP_INTERVAL_NS 14600000
#define POLL_TIMEOUT 45

long calculate_diff_time(struct timeval begin_time, struct timeval current_time);

int main(int argc, char *argv[])
{
    int listen_sockfd, send_sockfd;
    struct sockaddr_in client_addr, server_addr_to, server_addr_from;
    struct timeval begin_time, current_time;
    struct packet packet;
    struct packet ack_packet;
    long int ack_num = 0;
    char last = 0;
    char ack = 0;

    // read filename from command line argument
    if (argc != 2)
    {
        printf("Usage: ./client <filename>\n");
        return 1;
    }
    char *filename = argv[1];

    // Create a UDP socket for listening
    listen_sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (listen_sockfd < 0)
    {
        perror("Could not create listen socket");
        return 1;
    }

    // Create a UDP socket for sending
    send_sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (send_sockfd < 0)
    {
        perror("Could not create send socket");
        return 1;
    }

    // Configure the server address structure to which we will send data
    memset(&server_addr_to, 0, sizeof(server_addr_to));
    server_addr_to.sin_family = AF_INET;
    server_addr_to.sin_port = htons(SERVER_PORT_TO);
    server_addr_to.sin_addr.s_addr = inet_addr(SERVER_IP);

    // Configure the client address structure
    memset(&client_addr, 0, sizeof(client_addr));
    client_addr.sin_family = AF_INET;
    client_addr.sin_port = htons(CLIENT_PORT);
    client_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    // Bind the listen socket to the client address
    if (bind(listen_sockfd, (struct sockaddr *)&client_addr, sizeof(client_addr)) < 0)
    {
        perror("Bind failed");
        close(listen_sockfd);
        return 1;
    }

    // Open file for reading
    FILE *fp = fopen(filename, "rb");
    if (fp == NULL)
    {
        perror("Error opening file");
        close(listen_sockfd);
        close(send_sockfd);
        return 1;
    }

    struct pollfd listener_fd = {.fd = listen_sockfd, .events = POLLIN};

    rewind(fp);
    fseek(fp, 0L, SEEK_END);
    long long int size_of_file = ftell(fp);
    char *buffer = (char *)malloc(size_of_file + 1);
    rewind(fp);

    long int send_window_start = 0;
    long int packet_count = 1 + (size_of_file / (EFFECTIVE_PAYLOAD_CAPACITY - 1));
    long int next_packet = 0;
    double congestion_window = INITIAL_CONGESTION_WINDOW_SIZE;
    int retransmission_count = INITIAL_RETRANSMISSION_COUNT;
    short int previous_ack = -1;
    int slow_start_threshold = 1000000;
    int state = SLOW_START_STATE;
    int encountered_last = 0;

    for (;;)
    {
        congestion_window = packet_count < send_window_start + congestion_window ? packet_count - send_window_start : congestion_window;

        if (next_packet < send_window_start + congestion_window)
        {
            int send_buffer_size = EFFECTIVE_PAYLOAD_CAPACITY - 1;
            if (size_of_file <= (next_packet + 1) * (EFFECTIVE_PAYLOAD_CAPACITY - 1))
            {
                last = 1;
                send_buffer_size = size_of_file - next_packet * (EFFECTIVE_PAYLOAD_CAPACITY - 1);
            }

            long offset = next_packet * (EFFECTIVE_PAYLOAD_CAPACITY - 1);
            fseek(fp, offset, SEEK_SET);
            fread(buffer, send_buffer_size, 1, fp);

            buffer[send_buffer_size] = last ? '\0' : buffer[send_buffer_size];

            build_packet(&packet, next_packet, ack_num, last, ack, send_buffer_size, buffer);
            ssize_t char_count_sent = sendto(send_sockfd, &packet, sizeof(packet), 0, (struct sockaddr *)&server_addr_to, sizeof(server_addr_to));
            if (char_count_sent < 0)
            {
                perror("Error in sending packet, char_count_sent < 0");
            }
            if (send_window_start == next_packet)
            {
                gettimeofday(&begin_time, NULL);
            }
            ++next_packet;
        }

        int poll_count = poll(&listener_fd, 1, POLL_TIMEOUT);
        while (poll_count != 0)
        {
            if (poll_count == -1)
            {
                perror("Error in polling, poll_count == -1");
            }

            if ((recv(listen_sockfd, &ack_packet, sizeof(ack_packet) - 1, 0)) == -1)
            {
                printf("Receiving ack packet errored");
                break;
            }
            if (ack_packet.last)
            {
                encountered_last = 1;
                break;
            }

            if (send_window_start <= ack_packet.acknum)
            {
                send_window_start = ack_packet.acknum;
                retransmission_count = INITIAL_RETRANSMISSION_COUNT;
                switch (state)
                {
                case SLOW_START_STATE:
                    state = (congestion_window + 1) > slow_start_threshold ? CONGESTION_AVOIDANCE_STATE : SLOW_START_STATE;
                    ++congestion_window;
                    break;
                case CONGESTION_AVOIDANCE_STATE:
                    congestion_window += 1 / congestion_window;
                    break;
                case FAST_RECOVERY_STATE:
                    state = CONGESTION_AVOIDANCE_STATE;
                    congestion_window = slow_start_threshold;
                    break;
                default:
                    state = CONGESTION_AVOIDANCE_STATE;
                    congestion_window = slow_start_threshold;
                    break;
                }
            }
            else if (ack_packet.acknum == previous_ack)
            {
                congestion_window = state == FAST_RECOVERY_STATE ? congestion_window + 1 : congestion_window;
                ++retransmission_count;
                if (retransmission_count == MAX_RETRANSMISSION_THRESHOLD)
                {
                    last = 0;
                    next_packet = send_window_start;
                    retransmission_count = INITIAL_RETRANSMISSION_COUNT;
                    switch (state)
                    {
                    case SLOW_START_STATE:
                        state = FAST_RECOVERY_STATE;
                        break;
                    case CONGESTION_AVOIDANCE_STATE:
                        slow_start_threshold = congestion_window / 2;
                        congestion_window = slow_start_threshold + 3;
                        state = FAST_RECOVERY_STATE;
                        break;
                    case FAST_RECOVERY_STATE:
                        break;
                    default:
                        break;
                    }
                }
            }
            else
            {
                previous_ack = ack_packet.acknum;
            }
            next_packet = next_packet < send_window_start ? send_window_start : next_packet;
            gettimeofday(&begin_time, NULL);
            poll_count = poll(&listener_fd, 1, 0);
        }

        if (encountered_last == 1)
        {
            break;
        }

        gettimeofday(&current_time, NULL);
        long delta_ms = calculate_diff_time(begin_time, current_time);

        if (delta_ms > ACK_TIMEOUT_MS)
        {
            last = 0;
            next_packet = send_window_start;
            slow_start_threshold = congestion_window / 2;
            state = SLOW_START_STATE;
            congestion_window = INITIAL_CONGESTION_WINDOW_SIZE;
            retransmission_count = INITIAL_RETRANSMISSION_COUNT;
        }
        struct timespec delay;
        delay.tv_sec = 0;
        delay.tv_nsec = SLEEP_INTERVAL_NS;
        nanosleep(&delay, NULL);
    }

    fclose(fp);
    close(listen_sockfd);
    close(send_sockfd);
    // TODO: Potentially introduce delay to allow closing of server
    return 0;
}

long calculate_diff_time(struct timeval begin_time, struct timeval current_time)
{
    long delta_ms = 0;
    delta_ms = (current_time.tv_sec - begin_time.tv_sec) * 1000L;
    delta_ms += (current_time.tv_usec - begin_time.tv_usec) / 1000L;
    return delta_ms;
}