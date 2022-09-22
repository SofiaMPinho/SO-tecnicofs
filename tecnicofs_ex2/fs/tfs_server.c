#include "operations.h"
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <pthread.h>
#include <stdbool.h>
#include <signal.h>

#define FREE -1
#define ALL_TAKEN -1

typedef struct {
    char code;
    int session_id;
    int fhandle;
    int flags;
    size_t len;
    char name[NAME_SIZE];
    char *content;
    pthread_mutex_t mutex;
    int reading;
    int writing;
} buffer;

int sessions[S];
buffer *threads[S];
pthread_cond_t cond_prod[S];
pthread_cond_t cond_cons[S];
int fserv, mounted;

void initialize_sessions();
void empty_buffer(buffer *b);
void initialize_threads_buffers();
void *work(void *arg);
void process_input(buffer *b);
void process(buffer *b);
void thread_loop(buffer *b);
void mount_input(buffer *b);
void mount(buffer *b);
void unmount(buffer *b);
void open_file_input(buffer *b);
void open_file(buffer *b);
void close_file_input(buffer *b);
void close_file(buffer *b);
void write_file_input(buffer *b);
void write_file(buffer *b);
void read_file_input(buffer *b);
void read_file(buffer *b);
void shutdown_after_all_closed(buffer *b);
int open_function(const char *file, int flag);
int close_function(int fd);
int write_function(int fd, void *buf, size_t bytes);
int read_function(void *buf, size_t bytes);

buffer *create_buffer(int session_id) {
    buffer *b = malloc(sizeof(buffer));

    if(b == NULL)
        return NULL;

    b->session_id = session_id;
    empty_buffer(b);
    b->writing = TRUE;
    b->reading = FALSE;

    return b;
}

int main(int argc, char **argv) {

    if (argc < 2) {
        printf("Please specify the pathname of the server's pipe.\n");
        return 1;
    }

    char *pipename = argv[1];
    printf("Starting TecnicoFS server with pipe called %s\n", pipename);

    if(tfs_init() != 0){
        return -1;
    }

    signal(SIGPIPE, SIG_IGN);
    initialize_sessions();
    initialize_threads_buffers();
    mounted = TRUE;

    unlink(pipename);

    if(mkfifo(pipename, 0777) < 0)
        return -1;

    
    if((fserv = open_function(pipename, O_RDONLY)) == -1) 
        return -1;

    pthread_t tid[S];

    int *session_ids[S];

    for(int i = 0; i < S; i++) {
        int *session_id = malloc(sizeof(int));

        if(session_id == NULL)
            return -1;

        *session_id = i;
        if(pthread_create(&tid[i], NULL, work, (void *)session_id) != 0)
            return -1;
        session_ids[i] = session_id;
    }

    while(mounted) {
        char code;
        ssize_t rd = read(fserv, &code, sizeof(char));

        if(rd == -1 && mounted) {
            if(errno == EINTR)
                continue;
            if(errno == EPIPE)
                return -1;
        }
        else if(rd == 0 && mounted) {
            if(close_function(fserv) == -1)
                return -1;
            if((fserv = open_function(pipename, O_RDONLY)) == -1)
                return -1;

            continue;
        }
        else if(rd > 0 && code != 0 && mounted) {
            int session_id;

            if(code != TFS_OP_CODE_MOUNT) {
                if(read_function(&session_id, sizeof(int)) == -1)
                    return -1;
            }
            
            else {
                int found = FALSE;

                for(session_id = 0; session_id < S; session_id++) {
                    if(sessions[session_id] == FREE) {
                        found = TRUE;
                        sessions[session_id] = TAKEN;
                        break;
                    }    
                }

                if(!(found)) {
                    buffer *b = create_buffer(0);
                    int fcli, taken = ALL_TAKEN;

                    if(b == NULL)
                        return -1;

                    mount_input(b);
                    if((fcli = open_function(b->name, O_WRONLY)) == -1)
                        return -1;

                    write_function(fcli, &taken, sizeof(int)); //not necessary to treat error because client pipe will be closed either way
                    if(close_function(fcli) == -1)
                        return -1;
                    free(b);
                    continue;
                }
            }
            buffer *b = threads[session_id];
            pthread_mutex_lock(&b->mutex);
            while(b->reading && mounted) pthread_cond_wait(&cond_prod[b->session_id], &b->mutex);

            if(!(mounted)) {
                pthread_mutex_unlock(&b->mutex);
                pthread_mutex_destroy(&b->mutex);
                break;
            }

            b->writing = TRUE;
            b->code = code;
            b->session_id = session_id;

            process_input(b);

            b->writing = FALSE;
            pthread_mutex_unlock(&b->mutex);
            pthread_cond_signal(&cond_cons[b->session_id]);
        }
    }

    for(int i = 0; i < S; i++)
        pthread_join(tid[i], NULL);

    for(int i = 0; i < S; i++) {
        free(threads[i]);
        free(session_ids[i]);
    }

    return -1;
}

void initialize_sessions() {
    for(int i = 0; i < S; i++)
        sessions[i] = FREE;
}

void empty_buffer(buffer *b) {
    b->code = '\0';
    b->fhandle = 0;
    b->len = 0;
    b->flags = 0;
    for(int i = 0; i < NAME_SIZE; i++)
        b->name[i] = '\0';
    b->content = NULL;
}

void initialize_threads_buffers() {
    for(int session_id = 0; session_id < S; session_id++) {
        buffer *b = create_buffer(session_id);

        if(b == NULL)
            exit(EXIT_FAILURE);

        pthread_mutex_init(&b->mutex, NULL);
        pthread_cond_init(&cond_prod[b->session_id], NULL);
        pthread_cond_init(&cond_cons[b->session_id], NULL);

        threads[session_id] = b;
    }
}

void *work(void *arg) {
    int s = *((int*)arg);
    buffer *b = threads[s];

    while(mounted) {
        pthread_mutex_lock(&b->mutex);
        while(b->writing && mounted) pthread_cond_wait(&cond_cons[b->session_id], &b->mutex);

        if(!(mounted)) {
            pthread_mutex_unlock(&b->mutex);
            pthread_mutex_destroy(&b->mutex);
            break;
        }

        b->reading = TRUE;        
        process(b);

        empty_buffer(b);

        b->reading = FALSE;
        b->writing = TRUE;

        pthread_mutex_unlock(&b->mutex);
        pthread_cond_signal(&cond_prod[b->session_id]);
    }
    return 0;
}

void process_input(buffer *b) {
    char code = b->code;

    switch(code) {
        case TFS_OP_CODE_MOUNT:
            mount_input(b);
            break;
        case TFS_OP_CODE_UNMOUNT:
            break;
        case TFS_OP_CODE_OPEN:
            open_file_input(b);
            break;
        case TFS_OP_CODE_CLOSE:
            close_file_input(b);
            break;
        case TFS_OP_CODE_WRITE:
            write_file_input(b);
            break;
        case TFS_OP_CODE_READ:
            read_file_input(b);
            break;
        case TFS_OP_CODE_SHUTDOWN_AFTER_ALL_CLOSED:
            break;
        default:
            return;
    }    
}

void process(buffer *b) {
    char code = b->code;

    switch(code) {
        case TFS_OP_CODE_MOUNT:
            mount(b);
            break;
        case TFS_OP_CODE_UNMOUNT:
            unmount(b);
            break;
        case TFS_OP_CODE_OPEN:
            open_file(b);
            break;
        case TFS_OP_CODE_CLOSE:
            close_file(b);
            break;
        case TFS_OP_CODE_WRITE:
            write_file(b);
            break;
        case TFS_OP_CODE_READ:
            read_file(b);
            break;
        case TFS_OP_CODE_SHUTDOWN_AFTER_ALL_CLOSED:
            shutdown_after_all_closed(b);
            break;
        default:
            return;
    }
}

void mount_input(buffer *b) {
    char pipename[NAME_SIZE+1];
    int i;

    for(i = 0; i < NAME_SIZE; i++) {
        char c;
        if(read_function(&c, sizeof(char)) == -1)
            exit(EXIT_FAILURE);

        if(c == '\0')
            break;
        pipename[i] = c;
    }
    pipename[i] = '\0';

    for(; i < NAME_SIZE - 1; i++) {
        char c;
        if(read_function(&c, sizeof(char)) == -1)
            exit(EXIT_FAILURE);
    }

    strcpy(b->name, pipename);
} 

void mount(buffer *b) {
    int fcli;

    if((fcli = open_function(b->name, O_WRONLY)) == -1)
        exit(EXIT_FAILURE);

    sessions[b->session_id] = fcli;
    if(write_function(fcli, &b->session_id, sizeof(int)) == -1)
        unmount(b);
}

void unmount(buffer *b) {
    int fcli = sessions[b->session_id];

    if(close_function(fcli) == -1)
        exit(EXIT_FAILURE);
    sessions[b->session_id] = FREE;
}

void open_file_input(buffer *b) {
    char file_name[NAME_SIZE+1];
    int i;

    for(i = 0; i < NAME_SIZE; i++) {
        char c;
        if(read_function(&c, sizeof(char)) == -1)
            exit(EXIT_FAILURE);
        
        if(c == '\0')
            break;
        file_name[i] = c;
    }
    file_name[i] = '\0';

    for(; i < NAME_SIZE - 1; i++) {
        char c;
        if(read_function(&c, sizeof(char)) == -1)
            exit(EXIT_FAILURE);
    }
    strcpy(b->name, file_name);

    if(read_function(&b->flags, sizeof(int)) == -1)
        exit(EXIT_FAILURE);
}

void open_file(buffer *b) {
    int fcli, answer;

    fcli = sessions[b->session_id];

    answer = tfs_open(b->name, b->flags);

    if(write_function(fcli, &answer, sizeof(int)) == -1)
        unmount(b);
}

void close_file_input(buffer *b) {
    if(read_function(&b->fhandle, sizeof(int)) == -1)
        exit(EXIT_FAILURE);
} 

void close_file(buffer *b) {
    int answer, fcli;

    answer = tfs_close(b->fhandle);

    fcli = sessions[b->session_id];

    if(write_function(fcli, &answer, sizeof(int)) == -1)
        unmount(b);
}

void write_file_input(buffer *b) {
    if(read_function(&b->fhandle, sizeof(int)) == -1)
        exit(EXIT_FAILURE);
    if(read_function(&b->len, sizeof(size_t)) == -1)
        exit(EXIT_FAILURE);

    b->content = malloc(sizeof(b->content)*b->len);
    if(b->content == NULL)
        exit(EXIT_FAILURE);

    if(read_function(b->content, b->len) == -1)
        exit(EXIT_FAILURE);
}

void write_file(buffer *b) {
    int fcli;
    ssize_t answer;

    answer = tfs_write(b->fhandle, b->content, b->len);
    free(b->content);

    fcli = sessions[b->session_id];

    if(write_function(fcli, &answer, sizeof(ssize_t)) == -1)
        unmount(b);
}

void read_file_input(buffer *b) {
    if(read_function(&b->fhandle, sizeof(int)) == -1)
        exit(EXIT_FAILURE);
    if(read_function(&b->len, sizeof(size_t)) == -1)
        exit(EXIT_FAILURE);
}

void read_file(buffer *b) {
    int fcli;
    ssize_t answer;

    char readBuffer[b->len];

    answer = tfs_read(b->fhandle, readBuffer, b->len);
    fcli = sessions[b->session_id];

    if(write_function(fcli, &answer, sizeof(ssize_t)) == -1) {
        unmount(b);
        return;
    }

    if(answer == -1)
        return;

    if(write_function(fcli, readBuffer, (size_t)answer) == -1)
        unmount(b);
}

void shutdown_after_all_closed(buffer *b) {
    int fcli, answer;

    fcli = sessions[b->session_id];

    answer = tfs_destroy_after_all_closed();

    if(write_function(fcli, &answer, sizeof(int)) == -1)
        unmount(b);

    mounted = FALSE;

    for(int i = 0; i < S; i++) {
        pthread_cond_signal(&cond_cons[i]);
    }
    for(int i = 0; i < S; i++) {
        pthread_cond_signal(&cond_prod[i]);
    }
}

int open_function(const char *file, int flag) {
    int fd;
    while((fd = open(file, flag)) == -1) {
        if(errno == EINTR)
            continue;
        if(errno == EPIPE) 
            return -1;
    }
    return fd;
}

int close_function(int fd) {
    while(close(fd) == -1) {
        if(errno == EINTR)
            continue;
        if(errno == EPIPE) 
            return -1;
    }
    return 0;
}

int write_function(int fd, void *buf, size_t bytes) {
    ssize_t written;
    while((written = write(fd, buf, bytes)) == -1) {
        if(errno == EINTR)
            continue;
        if(errno == EPIPE) 
            return -1;
    }

    if(written < bytes)
        write_function(fd, (char*)buf+written, bytes-(size_t)written);

    return 0; 
}

int read_function(void *buf, size_t bytes) {
    ssize_t rd;
    while((rd = read(fserv, buf, bytes)) == -1) {
        if(errno == EINTR)
            continue;
        if(errno == EPIPE)
            return -1;
    }

    if(rd < bytes)
        read_function((char*)buf+rd, bytes-(size_t)rd);

    return 0;
}