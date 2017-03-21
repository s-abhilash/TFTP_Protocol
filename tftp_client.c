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
#define CLOSED 0
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
#define MAX_TIME_OUT 1000000
#define CMD_LEN 20

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
    STATE_RRQ,
    STATE_WRQ,
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
    struct sockaddr_in server_addr;
    socklen_t struct_len;
    char filename[FNAME_SIZE];
    int file_fd;
    long bytes_recv;
    int eof_indicator;
    short blocks_recv;
    short last_block_no;
} job_info_read_t;

typedef struct job_info_write
{
    struct sockaddr_in server_addr;
    socklen_t struct_len;
    char filename[FNAME_SIZE];
    int file_fd;
    long file_size;
    long bytes_sent;
    long bytes_rem;
    int eof_indicator;
    short blocks_sent;
    short last_block_no;
} job_info_write_t;

typedef struct rrq_packet
{
    short opcode;
    char filename[FNAME_SIZE];
    char mode[MODE_SIZE];
} rrq_packet_t;

typedef struct wrq_packet
{
    short opcode;
    char filename[FNAME_SIZE];
    char mode[MODE_SIZE];
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
    short count;
    short pad[DGRAM_SIZE - 6];
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
int send_a_read_request(state_t *state, int *connection, int *tftp_fd, fd_set *tftp_sock_set, job_info_read_t *job_read);
int send_a_write_request(state_t *state, int *connection, int *tftp_fd, fd_set *tftp_sock_set, job_info_write_t *job_write, data_packet_t *data_packet);
int read_handler(state_t *state, int *connection, int *tftp_fd, fd_set *tftp_sock_set, job_info_read_t *job_read, int *time_out);
int write_handler(state_t *state, int *connection, int *tftp_fd, fd_set *tftp_sock_set, job_info_write_t *job_write, int *time_out, data_packet_t *data_packet);
int server_match(struct sockaddr_in connected_server, struct sockaddr_in new_server);
int set_event(int tftp_fd, void *job, event_t event, int type, state_t state, data_packet_t *data_packet);

int main(int argc, char *argv[])
{
    int tftp_fd = -1;
    fd_set tftp_sock_set;
    struct sockaddr_in server_addr;
    struct timeval tv;
    socklen_t struct_len;
    char read_buffer[DGRAM_SIZE];
    int connection = CLOSED;
    int ret_val; 
    int time_out = RESET;
    int time_out_count = RESET;
    state_t state;
    event_t event;
    job_info_read_t job_read;
    job_info_write_t job_write;
    char cmd[CMD_LEN];
    char *cmd_ptr;
    char *filename;
    int idx;
    data_packet_t data_packet;
    struct addrinfo hints;
    struct addrinfo *res;
    int log_fd;

    hints.ai_flags = 0;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = 17;

    if (argc != 2)
    {
        printf("Usage : ./%s <host>\n", argv[0]);
        exit(0);
    }

    /* Resolve server address */
    getaddrinfo(argv[1], "tftp", &hints, &res);
    server_addr = *(struct sockaddr_in *)res->ai_addr;

    /* Change directory to TFTP CLIENT directory */
    if (chdir("client_storage") == -1)
    {
        printf("Please create a directory named \"client_storage\" before running the client\n");
        exit(0);
    }

    printf("tftp>> ");
    fgets(cmd, CMD_LEN, stdin);
    for (idx = 0; cmd[idx] != '\n'; idx++);
    cmd[idx] =  '\0';

    cmd_ptr = strtok(cmd, " ");
    if (strcmp(cmd_ptr, "get") == 0)
    {
        state = STATE_RRQ;
        filename = strtok(NULL, " ");
    }
    else if (strcmp(cmd_ptr, "put") == 0)
    {
        state = STATE_WRQ;
        filename = strtok(NULL, " ");
    }
    else
    {
        printf("Usage : get <filename>\n");
        printf("Usage : put <filename>\n");
        exit(0);
    }

#ifdef LOG
    log_fd = open("logger.txt", O_CREAT|O_WRONLY|O_TRUNC, S_IRUSR|S_IWUSR|S_IROTH|S_IWOTH);
    dup2(log_fd, 1);
    fchown(log_fd, 1000, 1000);
#endif
    /* Open a socket */
    if ((tftp_fd = socket(AF_INET, SOCK_DGRAM, 0)) < 0)
    {
        perror("socket");
        exit(0);
    }

    /* Initialize the set of sockets. */
    FD_ZERO (&tftp_sock_set);
    FD_SET (tftp_fd, &tftp_sock_set);

    /* Initialize timer to wait up to one second if connection established */
    tv.tv_sec = 1;
    tv.tv_usec = 0;

    connection = NEW_CONNECTION;
    /* Clear job structures */
    memset(&job_read, 0, sizeof(job_read));
    memset(&job_write, 0, sizeof(job_write));
    time_out = RESET;
    time_out_count = RESET;

    printf("\n###########################################\n");
    printf("#            Connecting to Sever           #\n");
    printf("#         This may take few seconds        #\n");
    printf("#             Please be patient            #\n");
    printf("###########################################\n\n");

    while (connection == ESTABILSHED || connection == NEW_CONNECTION)
    {
        if (connection == ESTABILSHED)
        {
#ifdef DEBUG
            if (state == STATE_READ)
            {
                printf("\n+--------------------------------------------+\n");
                printf("| Client Waiting for DATA Packet from server  |\n");
                printf("+---------------------------------------------+\n\n");
            }
            else
            {
                printf("\n+-------------------------------------------+\n");
                printf("| Client Waiting for ACK Packet from server  |\n");
                printf("+--------------------------------------------+\n\n");
            }
#endif

            tv.tv_sec = 1;
            tv.tv_usec = 0;
            if ((ret_val = select(FD_SETSIZE, &tftp_sock_set, NULL, NULL, &tv)) == -1)
            {
                perror("select");
                exit(0);
            }
            else if (!ret_val)
            {
                printf("Time out\n");
                time_out = SET;
                time_out_count++;
            }
        }
        if (time_out_count == MAX_TIME_OUT)
        {
            close(tftp_fd);

            /* Reset tftp socket */
            FD_ZERO (&tftp_sock_set);

            printf("ERROR : MAX TIMEOUT REACHED\n");
            printf("Connection closed with client\n");
            connection = CLOSED;
            continue;
        }

        switch (state)
        {

            case STATE_RRQ:
                job_read.server_addr = server_addr;
                job_read.struct_len = sizeof(struct sockaddr);
                strcpy(job_read.filename, filename);
                send_a_read_request(&state, &connection, &tftp_fd, &tftp_sock_set, &job_read);
                break;
            case STATE_WRQ:
                job_write.server_addr = server_addr;
                job_write.struct_len = sizeof(struct sockaddr);
                strcpy(job_write.filename, filename);
                send_a_write_request(&state, &connection, &tftp_fd, &tftp_sock_set, &job_write, &data_packet);
                break;
            case STATE_READ:
                read_handler(&state, &connection, &tftp_fd, &tftp_sock_set, &job_read, &time_out);
                break;
            case STATE_WRITE:
                write_handler(&state, &connection, &tftp_fd, &tftp_sock_set, &job_write, &time_out, &data_packet);
                break;

        } /* Switching between states */

    } /* For connection established */

    return 0;
}

int send_a_read_request(state_t *state, int *connection, int *tftp_fd, fd_set *tftp_sock_set, job_info_read_t *job_read)
{
    int bytes_sent = 0;
    int bytes_recv = 0;
    int recv_block_no;
    int ret_val;
    short int opcode;
    char read_buffer[DGRAM_SIZE];
    struct timeval tv;
    socklen_t struct_len;
    rrq_packet_t rrq_packet;
    ack_packet_t ack_packet;

    rrq_packet.opcode = htons(OPCODE_RRQ);
    strcpy(rrq_packet.filename, job_read->filename);
    strcpy(rrq_packet.mode, "netascii");
    struct_len = sizeof(struct sockaddr);


    bytes_sent = sendto(*tftp_fd, &rrq_packet, 2 + strlen(job_read->filename) + strlen("netascii") + 2, 0, (struct sockaddr *)&job_read->server_addr, job_read->struct_len);
    if (bytes_sent == -1)
    {
        perror("sendto");
        printf("LINE %d\n", __LINE__);
        exit(0); //TODO
    }

#ifdef DEBUG
    printf("******************RRQ PACKET SENT*****************\n");
    printf("******************%d Bytes SENT*****************\n", bytes_sent);
#endif
    /* Initialize timer to wait up to one second if connection established */
    tv.tv_sec = 1;
    tv.tv_usec = 0;

    if ((ret_val = select(FD_SETSIZE, tftp_sock_set, NULL, NULL, &tv)) == -1)
    {
        perror("select");
        exit(0);
    }
    else if (!ret_val)
    {
#ifdef DEBUG
        printf("ERROR : NO RESPONSE FROM SERVER\n");
        exit(0);
#endif
    }

    if ((bytes_recv = recvfrom(*tftp_fd, (void *)read_buffer, DGRAM_SIZE, 0, (struct sockaddr *)&job_read->server_addr, &job_read->struct_len)) == -1)
    {
        perror("recv");
        printf("LINE %d\n", __LINE__);
        exit(0);
    }

    opcode = ntohs(*(short int *)read_buffer);
    recv_block_no = ntohs(*(short int *)(read_buffer + 2));

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

    if (opcode == OPCODE_DATA && recv_block_no == 1)
    {
        if ((job_read->file_fd = open(job_read->filename, O_CREAT|O_TRUNC|O_WRONLY|O_EXCL, S_IRUSR|S_IWUSR|S_IROTH|S_IWOTH)) == -1)
        {
            perror("open");
            printf("LINE %d\n", __LINE__);
            exit(0);
        }
            
        chown(job_read->filename, 1000, 1000);

        /* Send Acknowledge packet */
        ack_packet.opcode = htons(OPCODE_ACK);
        ack_packet.block_no = htons(1);
        bytes_sent = sendto(*tftp_fd, &ack_packet, 4, 0, (struct sockaddr *)&job_read->server_addr, job_read->struct_len);
        if (bytes_sent == -1)
        {
            perror("sendto");
            printf("LINE %d\n", __LINE__);
            exit(0); //TODO
        }
#ifdef DEBUG
    printf("******************ACK PACKET SENT*****************\n");
    printf("******************%d Bytes SENT*****************\n", bytes_sent);
    printf("******************Block No. 1*****************\n");
#endif

        /* Store recived data packet to file */
        job_read->last_block_no = recv_block_no;
        job_read->blocks_recv++;
        job_read->bytes_recv += (bytes_recv - 4);
        write(job_read->file_fd, read_buffer + 4, bytes_recv - 4);

        if (bytes_recv < DGRAM_SIZE)
        {
            close(*tftp_fd);
            close(job_read->file_fd);
            printf("File transfer completed\n");
            printf("File %s recived succesfully\n", job_read->filename);
            printf("File size is %lu bytes\n", job_read->bytes_recv);
            /* Reset tftp sock set. */
            FD_ZERO (tftp_sock_set);

            *connection = CLOSED;
            return 0;
        }
        else
        {
            *state = STATE_READ;
            *connection = ESTABILSHED;
            return 1;
        }
    }
    else
    {
        if (opcode == OPCODE_ERROR)
        {
#ifdef DEBUG 
            printf("ERROR : MESSAGE FROM SERVER : %s\nCONNECTION CLOSED\n", read_buffer + 4);
#endif
        }
        else
        {
            printf("ILLEGAL OPERATION FROM SERVER \nCONNECTION CLOSED\n");
        }
        close(*tftp_fd);
        /* Reset tftp sock set. */
        FD_ZERO (tftp_sock_set);

        *connection = CLOSED;
        return 0;
    }
}

int send_a_write_request(state_t *state, int *connection, int *tftp_fd, fd_set *tftp_sock_set, job_info_write_t *job_write, data_packet_t *data_packet)
{
    int bytes_sent = 0;
    int bytes_recv = 0;
    int ret_val;
    int recv_block_no;
    short int opcode;
    char read_buffer[DGRAM_SIZE];
    struct timeval tv;
    socklen_t struct_len;
    wrq_packet_t wrq_packet;

    wrq_packet.opcode = htons(OPCODE_WRQ);
    strcpy(wrq_packet.filename, job_write->filename);
    strcpy(wrq_packet.mode, "netascii");
    struct_len = sizeof(struct sockaddr);


    bytes_sent = sendto(*tftp_fd, &wrq_packet, 2 + strlen(job_write->filename) + strlen("netascii") + 2, 0, (struct sockaddr *)&job_write->server_addr, job_write->struct_len);
    if (bytes_sent == -1)
    {
        perror("sendto");
        printf("LINE %d\n", __LINE__);
        exit(0); //TODO
    }

#ifdef DEBUG
    printf("******************WRQ PACKET SENT*****************\n");
    printf("******************%d Bytes SENT*****************\n", bytes_sent);
#endif

    /* Initialize timer to wait up to one second if connection established */
    tv.tv_sec = 1;
    tv.tv_usec = 0;


    if ((ret_val = select(FD_SETSIZE, tftp_sock_set, NULL, NULL, &tv)) == -1)
    {
        perror("select");
        exit(0);
    }
    else if (!ret_val)
    {
#ifdef DEBUG
        printf("ERROR : NO RESPONSE FROM SERVER\n");
        exit(0);
#endif
    }

    if ((bytes_recv = recvfrom(*tftp_fd, (void *)read_buffer, DGRAM_SIZE, 0, (struct sockaddr *)&job_write->server_addr, &job_write->struct_len)) == -1)
    {
        perror("recv");
        printf("LINE %d\n", __LINE__);
        exit(0);
    }

    opcode = ntohs(*(short int *)read_buffer);
    recv_block_no = ntohs(*(short int *)(read_buffer + 2));

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

    if (opcode == OPCODE_ACK && recv_block_no == 0)
    {
        if ((job_write->file_fd = open(job_write->filename, O_RDONLY)) == -1)
        {
            perror("open");
            printf("LINE %d\n", __LINE__);
            exit(0);
        }
            
        job_write->file_size = lseek(job_write->file_fd, 0, SEEK_END);
        lseek(job_write->file_fd, 0, SEEK_SET);
        job_write->bytes_rem = job_write->file_size;

        /* Send data packet */
        data_packet->opcode = htons(OPCODE_DATA);
        data_packet->block_no = htons(1);
        data_packet->data_size = read(job_write->file_fd, data_packet->data, DATA_SIZE);

        bytes_sent = sendto(*tftp_fd, data_packet, 4 + data_packet->data_size, 0, (struct sockaddr *)&job_write->server_addr, job_write->struct_len);
        if (bytes_sent == -1)
        {
            perror("sendto");
            printf("LINE %d\n", __LINE__);
            exit(0); //TODO
        }

#if DEBUG
        printf("******************DATA PACKET Sent****************\n");
        printf("******************%d Bytes Sent*****************\n", data_packet->data_size);
        printf("******************Block No. %hd*****************\n", ntohs(data_packet->block_no));
#endif

        job_write->last_block_no = 1;
        job_write->blocks_sent++;
        job_write->bytes_sent += data_packet->data_size;
        job_write->bytes_rem -= data_packet->data_size;

        if (data_packet->data_size < DATA_SIZE)
        {
            job_write->eof_indicator = SET;
        }

        *state = STATE_WRITE;
        *connection = ESTABILSHED;
        return 1;
    }
    else
    {
        if (opcode == OPCODE_ERROR)
        {
#ifdef DEBUG 
            printf("ERROR : MESSAGE FROM SERVER : %s\nCONNECTION CLOSED\n", read_buffer + 4);
#endif
        }
        else
        {
            printf("ILLEGAL OPERATION FROM SERVER \nCONNECTION CLOSED\n");
        }
        close(*tftp_fd);
        /* Reset tftp sock set. */
        FD_ZERO (tftp_sock_set);

        *connection = CLOSED;
        return 0;
    }
}

int write_handler(state_t *state, int *connection, int *tftp_fd, fd_set *tftp_sock_set, job_info_write_t *job_write, int *time_out, data_packet_t *data_packet)
{
    int bytes_recv = 0;
    int recv_block_no;
    short int opcode;
    char read_buffer[DGRAM_SIZE];
    struct sockaddr_in server_addr;
    socklen_t struct_len;

    struct_len = sizeof(struct sockaddr);
    if (*time_out)
    {
#ifdef DEBUG
        printf("READ TIME OUT\n");
        printf("EVENT_WRITE\n");
#endif
        if (!set_event(*tftp_fd, job_write, EVENT_WRITE, RETRANSMIT, STATE_WRITE, data_packet)) //TODO //FIXME
        {
        }
        *time_out = RESET;
        *state = STATE_WRITE;
        *connection = ESTABILSHED;
        return 1;
    }

    if ((bytes_recv = recvfrom(*tftp_fd, (void *)read_buffer, DGRAM_SIZE, 0, (struct sockaddr *)&server_addr, &struct_len)) == -1)
    {
        perror("recv");
        printf("LINE %d\n", __LINE__);
        exit(0);
    }

    if (!server_match(job_write->server_addr, server_addr))
    {
#ifdef DEBUG
        printf("Unknown server\n");
        printf("EVENT_ERROR\n");
#endif
        if (!set_event(*tftp_fd, &server_addr, EVENT_ERROR, UID, STATE_WRITE, NULL)) //TODO //FIXME
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
            if (!set_event(*tftp_fd, job_write, EVENT_ERROR, ILL, STATE_WRITE, NULL)) //TODO //FIXME
            {
            }
            close(*tftp_fd);
            close(job_write->file_fd);
            /* Reset tftp sock set. */
            FD_ZERO (tftp_sock_set);

            *connection = CLOSED;
            return 0;
        case OPCODE_WRQ:
#ifdef DEBUG
            printf("STATE_WRITE:OPCODE_WRQ\n");
#endif
            if (!set_event(*tftp_fd, job_write, EVENT_ERROR, ILL, STATE_WRITE, NULL)) //TODO //FIXME
            {
            }
            close(*tftp_fd);
            close(job_write->file_fd);
            /* Reset tftp sock set. */
            FD_ZERO (tftp_sock_set);
            *connection = CLOSED;

            return 0;

        case OPCODE_DATA:
#ifdef DEBUG
            printf("STATE_WRITE:OPCODE_DATA\n");
#endif
            if (!set_event(*tftp_fd, job_write, EVENT_ERROR, ILL, STATE_WRITE, NULL)) //TODO //FIXME
            {
            }
            close(*tftp_fd);
            close(job_write->file_fd);
            /* Reset tftp sock set. */
            FD_ZERO (tftp_sock_set);
            *connection = CLOSED;

            return 0;

        case OPCODE_ACK:
            if (job_write->eof_indicator == SET)
            {
                printf("File transfer completed\n");
                printf("File %s sent successfully\n", job_write->filename);
                printf("File size is %lu bytes\n", job_write->bytes_sent);
                printf("EVENT_IDLE\n");
                close(*tftp_fd);
                close(job_write->file_fd);
                /* Reset tftp sock set. */
                FD_ZERO (tftp_sock_set);
                *connection = CLOSED;
                return 1;
            }

            recv_block_no = ntohs(*(short *)(read_buffer + 2));

            //FIXME
            if (job_write->last_block_no - 1 == recv_block_no)
            {
                printf("STATE_WRITE:OPCODE_ACK\n");
                printf("EVENT_WRITE\n");
#if 1
                if (!set_event(*tftp_fd, job_write, EVENT_WRITE, RETRANSMIT, STATE_WRITE, data_packet)) //TODO //FIXME
                {
                }
#endif
                *state = STATE_WRITE;
                *connection = ESTABILSHED;
                return 1;
            }

            if (job_write->last_block_no == recv_block_no)
            {
#ifdef DEBUG
                printf("STATE_WRITE:OPCODE_ACK\n");
                printf("EVENT_WRITE\n");
#endif
                if (!set_event(*tftp_fd, job_write, EVENT_WRITE, 0, STATE_WRITE, data_packet)) //TODO //FIXME
                {
                }
                *state = STATE_WRITE;
                *connection = ESTABILSHED;
                return 1;
            }
            else 
            {
#ifdef DEBUG
                printf("STATE_WRITE:OPCODE_ACK\n");
                printf("EVENT_ERROR\n");
#endif
                if (!set_event(*tftp_fd, job_write, EVENT_ERROR, ILL, STATE_WRITE, NULL)) //TODO //FIXME
                {
                }
                close(*tftp_fd);
                close(job_write->file_fd);
                /* Reset tftp sock set. */
                FD_ZERO (tftp_sock_set);

                *connection = CLOSED;
                return 1;
            }
            return 1;

        case OPCODE_ERROR:
#ifdef DEBUG
            printf("STATE_WRITE:OPCODE_ERROR\n");
            printf("EVENT_ERROR\n");
#endif
            printf("ERROR : %s\n", read_buffer + 4);
            close(*tftp_fd);
            close(job_write->file_fd);
            /* Reset tftp sock set. */
            FD_ZERO (tftp_sock_set);
            *connection = CLOSED;

            return 1;

        default:
#ifdef DEBUG
            printf("STATE_WRITE:DEFAULT\n");
            printf("EVENT_ERROR\n");
#endif
            if (!set_event(*tftp_fd, job_write, EVENT_ERROR, ILL, STATE_WRITE, NULL)) //TODO //FIXME
            {
            }
            close(*tftp_fd);
            close(job_write->file_fd);
            /* Reset tftp sock set. */
            FD_ZERO (tftp_sock_set);
            *connection = CLOSED;
            return 0;

    } /* Switching between states */
}

int read_handler(state_t *state, int *connection, int *tftp_fd, fd_set *tftp_sock_set, job_info_read_t *job_read, int *time_out)
{
    int bytes_recv = 0;
    int recv_block_no;
    short int opcode;
    char read_buffer[DGRAM_SIZE];
    struct sockaddr_in server_addr;
    socklen_t struct_len;
    static int window = 0;

    struct_len = sizeof(struct sockaddr);
    if (*time_out)
    {
#ifdef DEBUG
        printf("READ TIME OUT\n");
        printf("EVENT_READ\n");
#endif
#if 0
        if (!set_event(*tftp_fd, job_read, EVENT_READ, RETRANSMIT, STATE_READ, NULL)) //TODO //FIXME
        {
        }
#endif
        *time_out = RESET;
        *state = STATE_READ;
        *connection = ESTABILSHED;
        //return 1;
    }

    if ((bytes_recv = recvfrom(*tftp_fd, (void *)read_buffer, DGRAM_SIZE, MSG_DONTWAIT, (struct sockaddr *)&server_addr, &struct_len)) == -1)
    {
        perror("recv");
        return 1;
        //exit(0);
    }

    if (!server_match(job_read->server_addr, server_addr))
    {
#ifdef DEBUG
        printf("Unknown sever\n");
        printf("EVENT_ERROR\n");
#endif
        if (!set_event(*tftp_fd, &server_addr, EVENT_ERROR, UID, STATE_READ, NULL)) //TODO //FIXME
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
            if (!set_event(*tftp_fd, job_read, EVENT_ERROR, ILL, STATE_READ, NULL)) //TODO //FIXME
            {
            }
            close(*tftp_fd);
            /* Reset tftp sock set. */
            FD_ZERO (tftp_sock_set);

            close(job_read->file_fd);
            remove(job_read->filename);
            *connection = CLOSED;
            return 0;
        case OPCODE_WRQ:
#ifdef DEBUG
            printf("STATE_READ:OPCODE_WRQ\n");
#endif
            if (!set_event(*tftp_fd, job_read, EVENT_ERROR, ILL, STATE_READ, NULL)) //TODO //FIXME
            {
            }
            close(*tftp_fd);
            /* Reset tftp sock set. */
            FD_ZERO (tftp_sock_set);

            close(job_read->file_fd);
            remove(job_read->filename);

            *connection = CLOSED;

            return 0;

        case OPCODE_DATA:
            recv_block_no = ntohs(*(short *)(read_buffer + 2));

            //FIXME
            if (job_read->last_block_no == recv_block_no)
            {
                printf("SAME BLOCK RECIEVED ############################\n");
                printf("STATE_READ:OPCODE_DATA\n");
                printf("EVENT_READ\n");
                if (!set_event(*tftp_fd, job_read, EVENT_READ, RETRANSMIT, STATE_READ, NULL)) //TODO //FIXME
                {
                }
                *state = STATE_READ;
                *connection = ESTABILSHED;
                return 1;
            }
            if (job_read->last_block_no + 1 == recv_block_no)
            {
#ifdef DEBUG
                printf("STATE_READ:OPCODE_DATA\n");
                printf("EVENT_READ\n");
#endif
                job_read->last_block_no = recv_block_no;
                if (!set_event(*tftp_fd, job_read, EVENT_READ, 0, STATE_READ, NULL)) //TODO //FIXME
                {
                }
                write(job_read->file_fd, read_buffer + 4, bytes_recv - 4);
                job_read->bytes_recv += (bytes_recv - 4);
                job_read->blocks_recv++;

                if (bytes_recv < DGRAM_SIZE)
                {
                    printf("File transfer completed\n");
                    printf("File %s recieved successfully\n", job_read->filename);
                    printf("File size is %lu bytes\n", job_read->bytes_recv);
                    printf("EVENT_IDLE\n");
                    close(job_read->file_fd);
                    close(*tftp_fd);
                    /* Reset tftp sock set. */
                    FD_ZERO (tftp_sock_set);
                    *connection = CLOSED;
                    return 1;
                }
                else 
                {
                    *state = STATE_READ;
                    *connection = ESTABILSHED;
                    return 1;
                }
            }
            else 
            {
#ifdef DEBUG
                printf("STATE_READ:OPCODE_DATA\n");
                printf("EVENT_ERROR\n");
#endif
                if (!set_event(*tftp_fd, job_read, EVENT_ERROR, ILL, STATE_READ, NULL)) //TODO //FIXME
                {
                }

                close(job_read->file_fd);
                remove(job_read->filename);

                *connection = CLOSED;
                return 1;
            }
            return 1;
        case OPCODE_ACK:
#ifdef DEBUG
            printf("STATE_READ:OPCODE_ACK\n");
            printf("EVENT_ERROR\n");
#endif
            if (!set_event(*tftp_fd, job_read, EVENT_ERROR, ILL, STATE_READ, NULL)) //TODO //FIXME
            {
            }
            close(*tftp_fd);
            /* Reset tftp sock set. */
            FD_ZERO (tftp_sock_set);

            close(job_read->file_fd);
            remove(job_read->filename);

            *connection = CLOSED;
            return 0;

        case OPCODE_ERROR:
#ifdef DEBUG
            printf("STATE_READ:OPCODE_ERROR\n");
            printf("EVENT_ERROR\n");
#endif
            printf("ERROR : %s\n", read_buffer + 4);
            close(*tftp_fd);
            /* Reset tftp sock set. */
            FD_ZERO (tftp_sock_set);

            close(job_read->file_fd);
            remove(job_read->filename);

            *connection = CLOSED;

            return 1;

        default:
#ifdef DEBUG
            printf("STATE_READ:DEFAULT\n");
            printf("EVENT_ERROR\n");
#endif
            if (!set_event(*tftp_fd, job_read, EVENT_ERROR, ILL, STATE_WRITE, NULL)) //TODO //FIXME
            {
            }
            close(*tftp_fd);
            /* Reset tftp sock set. */
            FD_ZERO (tftp_sock_set);

            close(job_read->file_fd);
            remove(job_read->filename);

            *connection = CLOSED;
            return 0;

    } /* Switching between states */
}
int set_event(int tftp_fd, void *job, event_t event, int type, state_t state, data_packet_t *data_packet)
{
    int bytes_sent;
    job_info_read_t *job_read;
    job_info_write_t *job_write;
    struct sockaddr_in server_addr;
    socklen_t struct_len;
    static short count = 1;

    error_packet_t err_packet;
    static ack_packet_t ack_packet;

    switch (event)
    {
        case EVENT_IDLE:
            return 1;
        case EVENT_WRITE:
            job_write = job;
            if (type != RETRANSMIT)
            {
                data_packet->opcode = htons(OPCODE_DATA);
                data_packet->block_no = htons(++job_write->last_block_no);
                data_packet->data_size = read(job_write->file_fd, data_packet->data, DATA_SIZE);
                if (data_packet->data_size == -1)
                {
                    perror("read from file");
                    printf("LINE %d\n", __LINE__);
                    exit(0); //TODO
                }
                if (data_packet->data_size != DATA_SIZE)
                {
                    job_write->eof_indicator = SET;
                }
            }

            bytes_sent = sendto(tftp_fd, data_packet, 4 + data_packet->data_size, 0, (struct sockaddr *)&job_write->server_addr, job_write->struct_len);
            if (bytes_sent == -1)
            {
                perror("sendto");
                printf("LINE %d\n", __LINE__);
                exit(0); //TODO
            }
            if (type != RETRANSMIT)
            {
                job_write->bytes_sent += (bytes_sent - 4);
                job_write->bytes_rem -= (bytes_sent - 4);
                job_write->blocks_sent++;
            }
#ifdef DEBUG
            if (type == 0)
            printf("******************DATA PACKET SENT****************\n");
            else if (type == RETRANSMIT)
            printf("***************DATA PACKET RETRANSMIT****************\n");
            printf("******************%d Bytes SENT *****************\n",bytes_sent);
            printf("******************Block Number %hd***************\n", job_write->last_block_no);
#endif
            return 1;

        case EVENT_READ:
            job_read = job;
            ack_packet.opcode = htons(OPCODE_ACK);
            ack_packet.block_no = htons(job_read->last_block_no);
            ack_packet.count = htons(++count);

            bytes_sent = sendto(tftp_fd, &ack_packet, DGRAM_SIZE, 0, (struct sockaddr *)&job_read->server_addr, job_read->struct_len);

            if (bytes_sent == -1)
            {
                perror("sendto");
                printf("LINE %d\n", __LINE__);
                exit(0); //TODO
            }

#ifdef DEBUG
            if (type == 0)
            printf("************* %d **ACK PACKET SENT****************\n", ntohs(ack_packet.count));
            else if (type == RETRANSMIT)
            printf("************* %d **ACK PACKET RETRANSMIT****************\n", ntohs(ack_packet.count));
            printf("******************%d Bytes SENT *****************\n",bytes_sent);
            printf("******************Block No %hd***************\n", job_read->last_block_no);
#endif
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
                    server_addr = job_read->server_addr;
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
                        server_addr = job_read->server_addr;
                        struct_len = job_read->struct_len;
                    }
                    else if (state == STATE_WRITE)
                    {
                        job_write = job;
                        server_addr = job_write->server_addr;
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
                        server_addr = job_read->server_addr;
                        struct_len = job_read->struct_len;
                    }
                    else if (state == STATE_WRITE)
                    {
                        job_write = job;
                        server_addr = job_write->server_addr;
                        struct_len = job_write->struct_len;
                    }
                    server_addr = *(struct sockaddr_in *)job;
                    struct_len = sizeof(struct sockaddr);
                    err_packet.err_code = htons(UID);
                    strcpy(err_packet.err_msg, err_msg[UID]);
                    err_packet.err_msg_size = strlen(err_msg[UID]) + 1;
                    break;
                case FAE:
#ifdef DEBUG
                    printf("ERROR : FILE ALREADY EXISTS\n");
#endif
                    job_write = job;
                    server_addr = job_write->server_addr;
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

            bytes_sent = sendto(tftp_fd, &err_packet, 4 + err_packet.err_msg_size, 0, (struct sockaddr *)&server_addr, struct_len);

            if (bytes_sent == -1)
            {
                perror("sendto");
                printf("LINE %d\n", __LINE__);
                exit(0); //TODO
            }

#ifdef DEBUG
            printf("******************ERROR PACKET SENT****************\n");
            printf("******************%d Bytes SENT *****************\n", bytes_sent);
#endif
            return 1;
    }
}

int server_match(struct sockaddr_in connected_server, struct sockaddr_in new_server)
{
    if (connected_server.sin_family == new_server.sin_family)
        if (connected_server.sin_port == new_server.sin_port)
            if (connected_server.sin_addr.s_addr == new_server.sin_addr.s_addr)
                return 1;
            else
                return 0;
}
