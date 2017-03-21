SRCS1 := tftp_client.c
SRCS2 := tftp_server.c
TRGT1 := tftp_client
TRGT2 := tftp_server

all : ${TRGT1} ${TRGT2}

${TRGT1} : ${SRCS1}
	gcc $^ -o $@

${TRGT2} : ${SRCS2}
	gcc $^ -o $@

clean :
	rm -f ${TRGT1} ${TRGT2}
