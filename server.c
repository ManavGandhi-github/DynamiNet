#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>

#include "utils.h"

#define SERVER_EXTRA_OPEN_TIME 10

int main()
{
    int listen_sockfd, send_sockfd;
    struct sockaddr_in server_addr, client_addr_from, client_addr_to;
    struct packet buffer;
    long int expected_seqnum = 0;
    struct packet ack_packet;

    // Create a UDP socket for sending
    send_sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (send_sockfd < 0)
    {
        perror("Could not create send socket");
        return 1;
    }

    // Create a UDP socket for listening
    listen_sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (listen_sockfd < 0)
    {
        perror("Could not create listen socket");
        return 1;
    }

    // Configure the server address structure
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(SERVER_PORT);
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    // Bind the listen socket to the server address
    if (bind(listen_sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0)
    {
        perror("Bind failed");
        close(listen_sockfd);
        return 1;
    }

    // Configure the client address structure to which we will send ACKs
    memset(&client_addr_to, 0, sizeof(client_addr_to));
    client_addr_to.sin_family = AF_INET;
    client_addr_to.sin_addr.s_addr = inet_addr(LOCAL_HOST);
    client_addr_to.sin_port = htons(CLIENT_PORT_TO);

    // Open the target file for writing (always write to output.txt)
    FILE *fp = fopen("output.txt", "wb");

    struct pollfd listener_fd = {.fd = listen_sockfd, .events = POLLIN};
    int has_finished = 0;

    build_packet(&ack_packet, 0, -1, 0, 1, 0, 0);
    for (;;)
    {
        if (recv(listen_sockfd, &buffer, sizeof(buffer) - 1, 0) == -1)
        {
            printf("Receiving packet errored");
            break;
        }
        short int is_expected = expected_seqnum == buffer.seqnum;
        if (!is_expected)
        {
            ssize_t char_count_sent = sendto(send_sockfd, &ack_packet, sizeof(ack_packet), 0, (struct sockaddr *)&client_addr_to, sizeof(client_addr_to));
            if (char_count_sent < 0)
            {
                perror("Error while sending ack");
            }
        }
        else
        {
            ++expected_seqnum;
            has_finished = buffer.last == 0 ? 0 : 1;
            fwrite(buffer.payload, buffer.length, 1, fp);
            build_packet(&ack_packet, 0, expected_seqnum, buffer.last, 1, 0, 0);
            ssize_t char_count_sent = sendto(send_sockfd, &ack_packet, sizeof(ack_packet), 0, (struct sockaddr *)&client_addr_to, sizeof(client_addr_to));
            if (char_count_sent < 0)
            {
                perror("Error while sending ack");
            }
        }

        if (has_finished && poll(&listener_fd, 1, SERVER_EXTRA_OPEN_TIME) == 0)
            break;
    }

    fclose(fp);
    close(listen_sockfd);
    close(send_sockfd);
    return 0;
}
