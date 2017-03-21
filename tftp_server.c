#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <string.h>
#include <signal.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netdb.h>
#include <sys/socket.h>
#include <errno.h>

#define TFTP_PORT 69
#define DGRAM_SIZE 516
#define DATA_SIZE 512
#define ACK_SIZE 4
#define CLOSED 0;
#define ESTABILSHED 1
#define NEW_CONNECTION 2
#define NO_OF_CLIENT 10
#define RUN_FOR_EVER 1
#define SET 1
#define RESET 0
#define FNAME_SIZE 30
#define ERR_MSG_SIZE 30
#define MODE_SIZE 10
#define RETRANSMIT 1
#define MAX_TIME_OUT 5

typedef enum error_code
{
    END,
    FNF,
    ACV,
    DFL,
    ILL,
    UID,
    FAE,
    NSU
} error_type_t;

typedef enum opcode
{
    OPCODE_RRQ = 1,
    OPCODE_WRQ,
    OPCODE_DATA,
    OPCODE_ACK,
    OPCODE_ERROR,
} opcode_t;

typedef enum state
{
    STATE_IDLE,
    STATE_READ,
    STATE_WRITE,
    STATE_DATA,
    STATE_ACK,
    STATE_ERROR,
} state_t;

typedef enum event
{
    EVENT_IDLE,
    EVENT_READ,
    EVENT_WRITE,
    EVENT_DATA,
    EVENT_ACK,
    EVENT_ERROR
} event_t;

typedef struct job_info_read
{
    struct sockaddr_in client_addr;
    socklen_t struct_len;
    char filename[FNAME_SIZE];
    int file_fd;
    long file_size;
    long bytes_sent;
    long bytes_rem;
    int eof_indicator;
    short blocks_sent;
    short last_block_no;
} job_info_read_t;

typedef struct job_info_write
{
    struct sockaddr_in client_addr;
    socklen_t struct_len;
    char filename[FNAME_SIZE];
    int file_fd;
    long bytes_recv;
    int eof_indicator;
    short blocks_recv;
    short last_block_no;
} job_info_write_t;

typedef struct rrq_packet
{
    short opcode;
    char *filename;
    char *mode;
} rrq_packet_t;

typedef struct wrq_packet
{
    short opcode;
    char *filename;
    char *mode;
} wrq_packet_t;

typedef struct data_packet
{
    short opcode;
    short block_no;
    int data[DATA_SIZE];
    int data_size;
} data_packet_t;

typedef struct ack_packet
{
    short opcode;
    short block_no;
} ack_packet_t;

typedef struct error_packet
{
    short opcode;
    short err_code;
    char err_msg[ERR_MSG_SIZE];
    int err_msg_size;
} error_packet_t;

static char *err_msg[] = {"Not defined", "File not found", "Access violation", "Disk full",
    "Illegal TFTP operartion", "Unknown TID", "FIle already exists", "No such user"};

/* Function prototypes */
int idle_handler(int sock_fd, state_t *state, int *connection, int *tftp_fd, fd_set *tftp_sock_set, job_info_read_t *job_read, job_info_write_t *job_write);
int read_handler(int sock_fd, state_t *state, int *connection, int *tftp_fd, fd_set *tftp_sock_set, job_info_read_t *job_read, int *time_out);
int write_handler(int sock_fd, state_t *state, int *connection, int *tftp_fd, fd_set *tftp_sock_set, job_info_write_t *job_write, int *time_out);
int client_match(struct sockaddr_in connected_client, struct sockaddr_in new_client);
int set_event(int tftp_fd, void *job, event_t event, int type, state_t state);

int main(void)
{
    int sock_fd = -1, tftp_fd = -1;
    fd_set sock_set, tftp_sock_set;
    struct sockaddr_in server_addr;
    struct sockaddr_in client_addr;
    struct timeval tv;
    socklen_t struct_len;
    char read_buffer[DGRAM_SIZE];
    int connection = CLOSED;
    int ret_val; 
    int time_out = RESET;
    int time_out_count = RESET;
    state_t state = STATE_IDLE;
    event_t event;
    job_info_read_t job_read;
    job_info_write_t job_write;

    /* Change directory to TFTP SERVER directory */
    if (chdir("server_storage") == -1)
    {
        printf("Please create a directory named \"server_storage\" before running the server\n");
        exit(0);
    }

    /* Open a socket */
    if ((sock_fd = socket(AF_INET, SOCK_DGRAM, 0)) < 0)
    {
        perror("socket");
        exit(0);
    }

    /* Bind to port 69 to accept connections */
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(TFTP_PORT);
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    struct_len = sizeof(struct sockaddr);

    if (bind(sock_fd, (struct sockaddr *)&server_addr, struct_len) == -1)
    {
        perror("bind");
        exit(0);
    }

    /* Initialize the set of sockets. */
    FD_ZERO (&sock_set);
    FD_SET (sock_fd, &sock_set);

    /* Initialize timer to wait up to one second if connection established */
    tv.tv_sec = 1;
    tv.tv_usec = 0;

    while (RUN_FOR_EVER)
    {
        printf("\n###########################################\n");
        printf("#    Server Waiting for new connection    #\n");
        printf("###########################################\n\n");

        if (select(FD_SETSIZE, &sock_set, NULL, NULL, NULL) < 0)
        {
            perror("select");
            exit(1);
        }
            connection = NEW_CONNECTION;
            state = STATE_IDLE;
            /* Clear job structures */
            memset(&job_read, 0, sizeof(job_read));
            memset(&job_write, 0, sizeof(job_write));
            time_out = RESET;
            time_out_count = RESET;

        while (connection == ESTABILSHED || connection == NEW_CONNECTION)
        {
            if (connection == ESTABILSHED)
            {
                if (state == STATE_READ)
                {
                    printf("\n+------------------------------------------+\n");
                    printf("|Server Waiting for ACK Packet from client  |\n");
                    printf("+-------------------------------------------+\n\n");
                }
                else
                {
                    printf("\n+------------------------------------------+\n");
                    printf("|Server Waiting for DATA Packet from client |\n");
                    printf("+-------------------------------------------+\n\n");
                }

                tv.tv_sec = 1;
                tv.tv_usec = 0;

                if ((ret_val = select(FD_SETSIZE, &tftp_sock_set, NULL, NULL, &tv)) == -1)
                {
                    perror("select");
                    exit(0);
                }
                else if (!ret_val)
                {
                    time_out = SET;
                    time_out_count++;
                }
            }
            if (time_out_count == MAX_TIME_OUT)
            {
                close(tftp_fd);

                /* Reset tftp socket */
                FD_ZERO (&tftp_sock_set);
#ifdef DEBUG
                printf("ERROR : MAX TIMEOUT REACHED\n");
                printf("Connection closed with client\n");
#endif
                connection = CLOSED;
                continue;
            }


            switch (state)
            {

                case STATE_IDLE:
                    idle_handler(sock_fd, &state, &connection, &tftp_fd, &tftp_sock_set, &job_read, &job_write);
                    break;
                case STATE_READ:
                    read_handler(sock_fd, &state, &connection, &tftp_fd, &tftp_sock_set, &job_read, &time_out);
                    break;
                case STATE_WRITE:
                    write_handler(sock_fd, &state, &connection, &tftp_fd, &tftp_sock_set, &job_write, &time_out);
                    break;

            } /* Switching between states */

        } /* For connection established */

    } /* Loop to run forever */

    return 0;
}

int idle_handler(int sock_fd, state_t *state, int *connection, int *tftp_fd, fd_set *tftp_sock_set, job_info_read_t *job_read, job_info_write_t *job_write)
{
    int bytes_recv = 0;
    short int opcode;
    char read_buffer[DGRAM_SIZE];
    struct sockaddr_in client_addr;
    socklen_t struct_len;

    struct_len = sizeof(struct sockaddr);
    if ((bytes_recv = recvfrom(sock_fd, (void *)read_buffer, DGRAM_SIZE, 0, (struct sockaddr *)&client_addr, &struct_len)) == -1)
    {
        perror("recv");
        exit(0);
    }
    opcode = ntohs(*(short int *)read_buffer);

#ifdef DEBUG 
    switch (opcode)
    {
        case OPCODE_RRQ:
            printf("******************RRQ PACKET RECIVED*****************\n");
        break;
        case OPCODE_WRQ:
            printf("******************WRQ PACKET RECIVED*****************\n");
        break;
        case OPCODE_DATA:
            printf("******************DATA PACKET RECIVED****************\n");
        break;
        case OPCODE_ACK:
            printf("******************ACK PACKET RECIVED*****************\n");
        break;
        case OPCODE_ERROR:
            printf("******************ERROR PACKET RECIVED***************\n");
        break;
    }
    printf("******************%d Bytes RECIVED*****************\n", bytes_recv);
#endif

    switch (opcode)
    {
        case OPCODE_RRQ:

            /* Open a socket */
            if ((*tftp_fd = socket(AF_INET, SOCK_DGRAM, 0)) < 0)
            {
                perror("creating tftp socket");
                exit(0);
            }

            /* Add tftp socket to fd_set */
            FD_ZERO (tftp_sock_set);
            FD_SET (*tftp_fd, tftp_sock_set);

            /* Get job info */
            job_read->client_addr = client_addr;
            job_read->struct_len = struct_len;
            strcpy(job_read->filename, read_buffer + 2);
            job_read->file_fd = open(job_read->filename, O_RDONLY);

            /* Check whether the requested file exists */
            if (job_read->file_fd == -1)
            {
                /* File does not exits, send a error packet */
                perror("open for reading");
                /* Send error msg */
#ifdef DEBUG
                printf("Request to send %s\n", job_read->filename);
                printf("File %s does not exists\n", job_read->filename);
                printf("STATE_IDLE:OPCODE_RRQ\n");
                printf("EVENT_ERROR\n");
#endif
            if (!set_event(*tftp_fd, job_read, EVENT_ERROR, FNF, STATE_IDLE)) //TODO //FIXME
            {
            }
                *state = STATE_IDLE;
                *connection = CLOSED;
                close(*tftp_fd);
                /* Reset tftp sock set. */
                FD_ZERO (tftp_sock_set);
                //while (1);
                return 1;
            }

            /* File exists, send first packet of data to client */
            job_read->file_size = lseek(job_read->file_fd, 0, SEEK_END);
#ifdef DEBUG
            printf("Request to send %s\n", job_read->filename);
            printf("File %s exists\n", job_read->filename);
            printf("Size of File %s = %lu bytes\n", job_read->filename, job_read->file_size);
#endif
            job_read->bytes_rem = job_read->file_size;
            lseek(job_read->file_fd, 0, SEEK_SET);


#ifdef DEBUG
            printf("STATE_IDLE:OPCODE_RRQ\n");
            printf("EVENT_READ\n");
#endif
            if (!set_event(*tftp_fd, job_read, EVENT_READ, 0, STATE_IDLE)) //TODO //FIXME
            {
            }
            *state = STATE_READ;
            *connection = ESTABILSHED;
            //while (1);
            return 1;

        case OPCODE_WRQ:
            /* Check whether the requested file already exists */
            /* Open a socket */
            if ((*tftp_fd = socket(AF_INET, SOCK_DGRAM, 0)) < 0)
            {
                perror("creating tftp socket");
                exit(0);
            }

            /* Add tftp socket to fd_set */
            FD_ZERO (tftp_sock_set);
            FD_SET (*tftp_fd, tftp_sock_set);

            /* Get job info */
            job_write->client_addr = client_addr;
            job_write->struct_len = struct_len;
            strcpy(job_write->filename, read_buffer + 2);
            job_write->file_fd = open(job_write->filename, O_WRONLY|O_CREAT|O_EXCL, S_IRUSR|S_IWUSR|S_IROTH|S_IWOTH|S_IRGRP);
            fchown(job_write->file_fd, 1000, 1000);

            /* Check whether the requested file exists */
            if (job_write->file_fd == -1)
            {
                /* File exits, send a error packet */
                perror("open for writing");
                /* Send error msg */
#ifdef DEBUG
                printf("Request to recv %s\n", job_write->filename);
                printf("File %s already exists\n", job_write->filename);
                printf("STATE_IDLE:OPCODE_WRQ\n");
                printf("EVENT_ERROR\n");
#endif
                if (!set_event(*tftp_fd, job_write, EVENT_ERROR, FAE, STATE_IDLE)) //TODO //FIXME
                {
                }
                *state = STATE_IDLE;
                *connection = CLOSED;
                close(*tftp_fd);
                /* Reset tftp sock set. */
                FD_ZERO (tftp_sock_set);
                return 1;
            }
#ifdef DEBUG
            printf("Request to recv %s\n", job_write->filename);
            printf("operation permitted\n");
#endif
            /* If file does not exists, send ack packet to client */
            /* If file exists, send a error packet */
#ifdef DEBUG
            printf("STATE_IDLE:OPCODE_WRQ\n");
            printf("EVENT_WRITE\n");
#endif
            if (!set_event(*tftp_fd, job_write, EVENT_WRITE, 0, STATE_READ)) //TODO //FIXME
            {
            }
            *state = STATE_WRITE;
            *connection = ESTABILSHED;

            return 1;

        case OPCODE_DATA:
#ifdef DEBUG
            printf("STATE_IDLE:OPCODE_DATA\n");
            printf("EVENT_ERROR\n");
#endif
            if (!set_event(*tftp_fd, job_read, EVENT_ERROR, ILL, STATE_IDLE)) //TODO //FIXME
            {
            }
            *state = STATE_IDLE;
            *connection = CLOSED;

            return 0;

        case OPCODE_ACK:
#ifdef DEBUG
            printf("STATE_IDLE:OPCODE_ACK\n");
            printf("EVENT_ERROR\n");
#endif
            if (!set_event(*tftp_fd, job_read, EVENT_ERROR, ILL, STATE_IDLE)) //TODO //FIXME
            {
            }
            *state = STATE_IDLE;
            *connection = CLOSED;

            return 0;

        case OPCODE_ERROR:
#ifdef DEBUG
            printf("STATE_IDLE:OPCODE_ERROR\n");
            printf("EVENT_ERROR\n");
#endif
            if (!set_event(*tftp_fd, job_read, EVENT_ERROR, ILL, STATE_IDLE)) //TODO //FIXME
            {
            }
            *state = STATE_IDLE;
            *connection = CLOSED;

            return 0;

        default:
#ifdef DEBUG
            printf("STATE_IDLE:DEFAULT\n");
            printf("EVENT_ERROR\n");
#endif
            if (!set_event(*tftp_fd, job_read, EVENT_ERROR, ILL, STATE_IDLE)) //TODO //FIXME
            {
            }
            *state = STATE_IDLE;
            *connection = CLOSED;

            return 0;

    } /* Switching between states */
}

int read_handler(int sock_fd, state_t *state, int *connection, int *tftp_fd, fd_set *tftp_sock_set, job_info_read_t *job_read, int *time_out)
{
    int bytes_recv = 0;
    int recv_block_no;
    short int opcode;
    char read_buffer[DGRAM_SIZE];
    struct sockaddr_in client_addr;
    socklen_t struct_len;

    struct_len = sizeof(struct sockaddr);
    if (*time_out)
    {
#ifdef DEBUG
        printf("READ TIME OUT\n");
        printf("EVENT_READ\n");
#endif
        if (!set_event(*tftp_fd, job_read, EVENT_READ, RETRANSMIT, STATE_READ)) //TODO //FIXME
        {
        }
        *time_out = RESET;
        *state = STATE_READ;
        *connection = ESTABILSHED;
        //return 1;
    }

    if ((bytes_recv = recvfrom(*tftp_fd, (void *)read_buffer, DGRAM_SIZE, MSG_DONTWAIT, (struct sockaddr *)&client_addr, &struct_len)) == -1)
    {
        perror("recv");
        //exit(0);
        return 1;
    }

    if (!client_match(job_read->client_addr, client_addr))
    {
#ifdef DEBUG
        printf("Unknown client\n");
        printf("EVENT_ERROR\n");
#endif
        if (!set_event(*tftp_fd, &client_addr, EVENT_ERROR, UID, STATE_READ)) //TODO //FIXME
        {
        }

        *state = STATE_READ;
        *connection = ESTABILSHED;
        return 0;
    }

    opcode = ntohs(*(short int *)read_buffer);

#ifdef DEBUG 
    switch (opcode)
    {
        case OPCODE_RRQ:
            printf("******************RRQ PACKET RECIVED*****************\n");
        break;
        case OPCODE_WRQ:
            printf("******************WRQ PACKET RECIVED*****************\n");
        break;
        case OPCODE_DATA:
            printf("******************DATA PACKET RECIVED****************\n");
        break;
        case OPCODE_ACK:
            printf("******************ACK PACKET RECIVED*****************\n");
        break;
        case OPCODE_ERROR:
            printf("******************ERROR PACKET RECIVED***************\n");
        break;
    }
    printf("******************%d Bytes RECIVED*****************\n", bytes_recv);
    printf("******************Block No. %hd*****************\n", ntohs(*(short *)(read_buffer + 2)));
#endif

    switch (opcode)
    {
        case OPCODE_RRQ:
#ifdef DEBUG
            printf("STATE_READ:OPCODE_RRQ\n");
            printf("EVENT_ERROR\n");
#endif
            if (!set_event(*tftp_fd, job_read, EVENT_ERROR, ILL, STATE_READ)) //TODO //FIXME
            {
            }
            close(*tftp_fd);
            /* Reset tftp sock set. */
            FD_ZERO (tftp_sock_set);

            *state = STATE_IDLE;
            *connection = CLOSED;
            return 0;
        case OPCODE_WRQ:
#ifdef DEBUG
            printf("STATE_READ:OPCODE_WRQ\n");
#endif
            if (!set_event(*tftp_fd, job_read, EVENT_ERROR, ILL, STATE_READ)) //TODO //FIXME
            {
            }
            close(*tftp_fd);
            /* Reset tftp sock set. */
            FD_ZERO (tftp_sock_set);
            *state = STATE_IDLE;
            *connection = CLOSED;

            return 0;

        case OPCODE_DATA:
#ifdef DEBUG
            printf("STATE_READ:OPCODE_DATA\n");
#endif
            if (!set_event(*tftp_fd, job_read, EVENT_ERROR, ILL, STATE_READ)) //TODO //FIXME
            {
            }
            close(*tftp_fd);
            /* Reset tftp sock set. */
            FD_ZERO (tftp_sock_set);
            *state = STATE_IDLE;
            *connection = CLOSED;

            return 0;

        case OPCODE_ACK:
            if (job_read->eof_indicator == SET)
            {
#ifdef DEBUG
                printf("File transfer completed\n");
                printf("File %s sent successfully\n", job_read->filename);
                printf("File size id %lu bytes\n", job_read->bytes_sent);
                printf("EVENT_IDLE\n");
#endif
                close(*tftp_fd);
                /* Reset tftp sock set. */
                FD_ZERO (tftp_sock_set);
                *state = STATE_IDLE;
                *connection = CLOSED;
                return 1;
            }

            recv_block_no = ntohs(*(short *)(read_buffer + 2));

            if (job_read->last_block_no - 1 == recv_block_no)
            {
#ifdef DEBUG
                printf("STATE_READ:OPCODE_ACK\n");
                printf("EVENT_READ\n");
#endif
#if 0
                if (!set_event(*tftp_fd, job_read, EVENT_READ, RETRANSMIT, STATE_READ)) //TODO //FIXME
                {
                }
                *state = STATE_READ;
                *connection = ESTABILSHED;
                return 1;
#endif
            }
            else if (job_read->last_block_no == recv_block_no)
            {
#ifdef DEBUG
                printf("STATE_READ:OPCODE_ACK\n");
                printf("EVENT_READ\n");
#endif
                if (!set_event(*tftp_fd, job_read, EVENT_READ, 0, STATE_READ)) //TODO //FIXME
                {
                }
                *state = STATE_READ;
                *connection = ESTABILSHED;
                return 1;
            }
            else 
            {
#ifdef DEBUG
                printf("STATE_READ:OPCODE_ACK\n");
                printf("EVENT_ERROR\n");
#endif
                if (!set_event(*tftp_fd, job_read, EVENT_ERROR, ILL, STATE_READ)) //TODO //FIXME
                {
                }
                close(*tftp_fd);
                /* Reset tftp sock set. */
                FD_ZERO (tftp_sock_set);

                *state = STATE_IDLE;
                *connection = CLOSED;
                return 1;
            }
            return 1;

        case OPCODE_ERROR:
#ifdef DEBUG
            printf("STATE_READ:OPCODE_ERROR\n");
            printf("EVENT_ERROR\n");
#endif
            printf("ERROR : %s\n", read_buffer + 4);
            close(*tftp_fd);
            /* Reset tftp sock set. */
            FD_ZERO (tftp_sock_set);
            *state = STATE_IDLE;
            *connection = CLOSED;

            return 1;

        default:
#ifdef DEBUG
            printf("STATE_READ:DEFAULT\n");
            printf("EVENT_ERROR\n");
#endif
            if (!set_event(*tftp_fd, job_read, EVENT_ERROR, ILL, STATE_READ)) //TODO //FIXME
            {
            }
            close(*tftp_fd);
            /* Reset tftp sock set. */
            FD_ZERO (tftp_sock_set);

            *state = STATE_IDLE;
            *connection = CLOSED;
            return 0;

    } /* Switching between states */
}

int write_handler(int sock_fd, state_t *state, int *connection, int *tftp_fd, fd_set *tftp_sock_set, job_info_write_t *job_write, int *time_out)
{
    int bytes_recv = 0;
    int recv_block_no;
    short int opcode;
    char read_buffer[DGRAM_SIZE];
    struct sockaddr_in client_addr;
    socklen_t struct_len;

    struct_len = sizeof(struct sockaddr);
    if (*time_out)
    {
#ifdef DEBUG
        printf("READ TIME OUT\n");
        printf("EVENT_READ\n");
#endif
        if (!set_event(*tftp_fd, job_write, EVENT_WRITE, RETRANSMIT, STATE_WRITE)) //TODO //FIXME
        {
        }
        *time_out = RESET;
        *state = STATE_WRITE;
        *connection = ESTABILSHED;
        return 1;
    }

    if ((bytes_recv = recvfrom(*tftp_fd, (void *)read_buffer, DGRAM_SIZE, 0, (struct sockaddr *)&client_addr, &struct_len)) == -1)
    {
        perror("recv");
        exit(0);
    }

    if (!client_match(job_write->client_addr, client_addr))
    {
#ifdef DEBUG
        printf("Unknown client\n");
        printf("EVENT_ERROR\n");
#endif
        if (!set_event(*tftp_fd, &client_addr, EVENT_ERROR, UID, STATE_WRITE)) //TODO //FIXME
        {
        }

        *state = STATE_WRITE;
        *connection = ESTABILSHED;
        return 0;
    }

    opcode = ntohs(*(short int *)read_buffer);

#ifdef DEBUG 
    switch (opcode)
    {
        case OPCODE_RRQ:
            printf("******************RRQ PACKET RECIVED*****************\n");
        break;
        case OPCODE_WRQ:
            printf("******************WRQ PACKET RECIVED*****************\n");
        break;
        case OPCODE_DATA:
            printf("******************DATA PACKET RECIVED****************\n");
        break;
        case OPCODE_ACK:
            printf("******************ACK PACKET RECIVED*****************\n");
        break;
        case OPCODE_ERROR:
            printf("******************ERROR PACKET RECIVED***************\n");
        break;
    }
    printf("******************%d Bytes RECIVED*****************\n", bytes_recv);
    printf("******************Block No. %hd*****************\n", ntohs(*(short *)(read_buffer + 2)));
#endif

    switch (opcode)
    {
        case OPCODE_RRQ:
#ifdef DEBUG
            printf("STATE_WRITE:OPCODE_RRQ\n");
            printf("EVENT_ERROR\n");
#endif
            if (!set_event(*tftp_fd, job_write, EVENT_ERROR, ILL, STATE_WRITE)) //TODO //FIXME
            {
            }
            close(*tftp_fd);
            /* Reset tftp sock set. */
            FD_ZERO (tftp_sock_set);

            close(job_write->file_fd);
            remove(job_write->filename);
            *state = STATE_IDLE;
            *connection = CLOSED;
            return 0;
        case OPCODE_WRQ:
#ifdef DEBUG
            printf("STATE_WRITE:OPCODE_WRQ\n");
#endif
            if (!set_event(*tftp_fd, job_write, EVENT_ERROR, ILL, STATE_WRITE)) //TODO //FIXME
            {
            }
            close(*tftp_fd);
            /* Reset tftp sock set. */
            FD_ZERO (tftp_sock_set);

            close(job_write->file_fd);
            remove(job_write->filename);

            *state = STATE_IDLE;
            *connection = CLOSED;

            return 0;

        case OPCODE_DATA:
            recv_block_no = ntohs(*(short *)(read_buffer + 2));
            if (job_write->last_block_no == recv_block_no)
            {
#ifdef DEBUG
                printf("STATE_WRITE:OPCODE_DATA\n");
                printf("EVENT_WRITE\n");
#endif
                if (!set_event(*tftp_fd, job_write, EVENT_WRITE, RETRANSMIT, STATE_WRITE)) //TODO //FIXME
                {
                }
                *state = STATE_WRITE;
                *connection = ESTABILSHED;
                return 1;
            }
            else if (job_write->last_block_no + 1 == recv_block_no)
            {
#ifdef DEBUG
                printf("STATE_WRITE:OPCODE_DATA\n");
                printf("EVENT_WRITE\n");
#endif
                job_write->last_block_no = recv_block_no;
                if (!set_event(*tftp_fd, job_write, EVENT_WRITE, 0, STATE_WRITE)) //TODO //FIXME
                {
                }
                write(job_write->file_fd, read_buffer + 4, bytes_recv - 4);
                job_write->bytes_recv += (bytes_recv - 4);

                if (bytes_recv < DGRAM_SIZE)
                {
#ifdef DEBUG
                    printf("File transfer completed\n");
                    printf("File %s recieved successfully\n", job_write->filename);
                    printf("File size is %lu bytes\n", job_write->bytes_recv);
                    printf("EVENT_IDLE\n");
#endif
                    close(job_write->file_fd);
                    close(*tftp_fd);
                    /* Reset tftp sock set. */
                    FD_ZERO (tftp_sock_set);
                    *state = STATE_IDLE;
                    *connection = CLOSED;
                    return 1;
                }
                else 
                {
                    *state = STATE_WRITE;
                    *connection = ESTABILSHED;
                    return 1;
                }
            }
            else 
            {
#ifdef DEBUG
                printf("STATE_WRITE:OPCODE_DATA\n");
                printf("EVENT_ERROR\n");
#endif
                if (!set_event(*tftp_fd, job_write, EVENT_ERROR, ILL, STATE_WRITE)) //TODO //FIXME
                {
                }

                close(job_write->file_fd);
                remove(job_write->filename);

                *state = STATE_IDLE;
                *connection = CLOSED;
                return 1;
            }
            return 1;
        case OPCODE_ACK:
#ifdef DEBUG
            printf("STATE_WRITE:OPCODE_ACK\n");
            printf("EVENT_ERROR\n");
#endif
            if (!set_event(*tftp_fd, job_write, EVENT_ERROR, ILL, STATE_WRITE)) //TODO //FIXME
            {
            }
            close(*tftp_fd);
            /* Reset tftp sock set. */
            FD_ZERO (tftp_sock_set);

            close(job_write->file_fd);
            remove(job_write->filename);

            *state = STATE_IDLE;
            *connection = CLOSED;
            return 0;

        case OPCODE_ERROR:
#ifdef DEBUG
            printf("STATE_WRITE:OPCODE_ERROR\n");
            printf("EVENT_ERROR\n");
#endif
            printf("ERROR : %s\n", read_buffer + 4);
            close(*tftp_fd);
            /* Reset tftp sock set. */
            FD_ZERO (tftp_sock_set);

            close(job_write->file_fd);
            remove(job_write->filename);

            *state = STATE_IDLE;
            *connection = CLOSED;

            return 1;

        default:
#ifdef DEBUG
            printf("STATE_WRITE:DEFAULT\n");
            printf("EVENT_ERROR\n");
#endif
            if (!set_event(*tftp_fd, job_write, EVENT_ERROR, ILL, STATE_WRITE)) //TODO //FIXME
            {
            }
            close(*tftp_fd);
            /* Reset tftp sock set. */
            FD_ZERO (tftp_sock_set);

            close(job_write->file_fd);
            remove(job_write->filename);

            *state = STATE_IDLE;
            *connection = CLOSED;
            return 0;

    } /* Switching between states */
}
int set_event(int tftp_fd, void *job, event_t event, int type, state_t state)
{
    int bytes_sent;
    job_info_read_t *job_read;
    job_info_write_t *job_write;
    struct sockaddr_in client_addr;
    socklen_t struct_len;

    static data_packet_t data_packet;
    error_packet_t err_packet;
    static ack_packet_t ack_packet;

    switch (event)
    {
        case EVENT_IDLE:
            return 1;
        case EVENT_READ:
            job_read = job;
            if (type != RETRANSMIT)
            {
                data_packet.opcode = htons(OPCODE_DATA);
                data_packet.block_no = htons(++job_read->last_block_no);
                data_packet.data_size = read(job_read->file_fd, data_packet.data, DATA_SIZE);
                if (data_packet.data_size == -1)
                {
                    perror("read from file");
                    printf("LINE %d\n", __LINE__);
                    exit(0); //TODO
                }
                if (data_packet.data_size != DATA_SIZE)
                {
                    job_read->eof_indicator = SET;
                }
            }

            bytes_sent = sendto(tftp_fd, &data_packet, 4 + data_packet.data_size, 0, (struct sockaddr *)&job_read->client_addr, job_read->struct_len);
            if (bytes_sent == -1)
            {
                perror("sendto");
                printf("LINE %d\n", __LINE__);
                exit(0); //TODO
            }
            if (type != RETRANSMIT)
            {
                job_read->bytes_sent += (bytes_sent - 4);
                job_read->bytes_rem -= (bytes_sent - 4);
                job_read->blocks_sent++;
            }
#ifdef DEBUG
            if (type == 0)
            printf("******************DATA PACKET SENT****************\n");
            else if (type == 1)
            printf("***************DATA PACKET RETRANSMIT****************\n");
            printf("******************%d Bytes SENT *****************\n",bytes_sent);
            printf("******************Block Number %hd***************\n", job_read->last_block_no);
#endif
            return 1;
        case EVENT_WRITE:
            job_write = job;
            if (type != RETRANSMIT)
            {
                ack_packet.opcode = htons(OPCODE_ACK);
                ack_packet.block_no = htons(job_write->last_block_no);
            }

            bytes_sent = sendto(tftp_fd, &ack_packet, 4, 0, (struct sockaddr *)&job_write->client_addr, job_write->struct_len);

            if (bytes_sent == -1)
            {
                perror("sendto");
                printf("LINE %d\n", __LINE__);
                exit(0); //TODO
            }

            printf("****************ACK PACKET SENT****************\n");
            printf("******************%d Bytes SENT *****************\n",bytes_sent);
            printf("******************Block No %hd***************\n", job_write->last_block_no);
            return 1;
        case EVENT_ACK:
            return 1;
        case EVENT_DATA:
            return 1;
        case EVENT_ERROR:
            err_packet.opcode = htons(OPCODE_ERROR);

            switch (type)
            {
                case END:
#ifdef DEBUG
                    printf("ERROR : NOT DEFINED\n");
#endif
                    err_packet.err_code = htons(END);
                    strcpy(err_packet.err_msg, err_msg[END]);
                    err_packet.err_msg_size = strlen(err_msg[END]) + 1;
                    break;
                case FNF:
#ifdef DEBUG
                    printf("ERROR : FILE NOT FOUND\n");
#endif
                    job_read = job;
                    client_addr = job_read->client_addr;
                    struct_len = job_read->struct_len;
                    err_packet.err_code = htons(FNF);
                    strcpy(err_packet.err_msg, err_msg[FNF]);
                    err_packet.err_msg_size = strlen(err_msg[FNF]) + 1;
                    break;
                case ACV:
#ifdef DEBUG
                    printf("ERROR : ACCESS VIOLATION\n");
#endif
                    err_packet.err_code = htons(ACV);
                    strcpy(err_packet.err_msg, err_msg[ACV]);
                    err_packet.err_msg_size = strlen(err_msg[ACV]) + 1;
                    break;
                case DFL:
#ifdef DEBUG
                    printf("ERROR : DISK FULL\n");
#endif
                    err_packet.err_code = htons(DFL);
                    strcpy(err_packet.err_msg, err_msg[DFL]);
                    err_packet.err_msg_size = strlen(err_msg[DFL]) + 1;
                    break;
                case ILL:
#ifdef DEBUG
                    printf("ERROR : ILLEGAL INSTRUCTION\n");
#endif
                    if (state == STATE_READ)
                    {
                        job_read = job;
                        client_addr = job_read->client_addr;
                        struct_len = job_read->struct_len;
                    }
                    else if (state == STATE_WRITE)
                    {
                        job_write = job;
                        client_addr = job_write->client_addr;
                        struct_len = job_write->struct_len;
                    }
                    err_packet.err_code = htons(ILL);
                    strcpy(err_packet.err_msg, err_msg[ILL]);
                    err_packet.err_msg_size = strlen(err_msg[ILL]) + 1;
                    break;
                case UID:
#ifdef DEBUG
                    printf("ERROR : UNKNOWN USER ID\n");
#endif
                    if (state == STATE_READ)
                    {
                        job_read = job;
                        client_addr = job_read->client_addr;
                        struct_len = job_read->struct_len;
                    }
                    else if (state == STATE_WRITE)
                    {
                        job_write = job;
                        client_addr = job_write->client_addr;
                        struct_len = job_write->struct_len;
                    }
                    err_packet.err_code = htons(UID);
                    strcpy(err_packet.err_msg, err_msg[UID]);
                    err_packet.err_msg_size = strlen(err_msg[UID]) + 1;
                    break;
                case FAE:
#ifdef DEBUG
                    printf("ERROR : FILE ALREADY EXISTS\n");
#endif
                    job_write = job;
                    client_addr = job_write->client_addr;
                    struct_len = job_write->struct_len;
                    err_packet.err_code = htons(FAE);
                    strcpy(err_packet.err_msg, err_msg[FAE]);
                    err_packet.err_msg_size = strlen(err_msg[FAE]) + 1;
                    break;
                case NSU:
#ifdef DEBUG
                    printf("ERROR : NO SUCH USER\n");
#endif
                    err_packet.err_code = htons(NSU);
                    strcpy(err_packet.err_msg, err_msg[NSU]);
                    err_packet.err_msg_size = strlen(err_msg[NSU]) + 1;
                    break;
            }
            bytes_sent = sendto(tftp_fd, &err_packet, 4 + err_packet.err_msg_size, 0, (struct sockaddr *)&client_addr, struct_len);
            if (bytes_sent == -1)
            {
                perror("sendto");
                printf("LINE %d\n", __LINE__);
                exit(0); //TODO
            }
#ifdef DEBUG
            printf("******************ERROR PACKET SENT****************\n");
            printf("******************%d Bytes SENT *****************\n",bytes_sent);
#endif
            return 1;

    }
}

int client_match(struct sockaddr_in connected_client, struct sockaddr_in new_client)
{
    if (connected_client.sin_family == new_client.sin_family)
        if (connected_client.sin_port == new_client.sin_port)
            if (connected_client.sin_addr.s_addr == new_client.sin_addr.s_addr)
                return 1;
            else
                return 0;
}
