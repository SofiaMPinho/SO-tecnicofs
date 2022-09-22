#include "tecnicofs_client_api.h"

#define ALL_TAKEN -1

int session_id, fcli, fserv;
char const *client_pipe;

int tfs_mount(char const *client_pipe_path, char const *server_pipe_path) {
    int code = TFS_OP_CODE_MOUNT;
    char name[NAME_SIZE], message[1+NAME_SIZE];
    client_pipe = client_pipe_path;
    
    unlink(client_pipe_path);

    if((fserv = open_function(server_pipe_path, O_WRONLY)) == -1) 
        return -1;
    
    if(mkfifo(client_pipe_path, 0777) < 0)
        return -1;

    strcpy(name, client_pipe_path);

    for(size_t i = strlen(name); i < NAME_SIZE; i++)
        name[i] = '\0';

    memcpy(message, &code, sizeof(char));
    memcpy(message+1, name, NAME_SIZE);

    if(write_function(message, 1+NAME_SIZE) == -1)
        return -1;

    if((fcli = open_function(client_pipe_path, O_RDONLY)) == -1) {
        return -1;
    } 

    if(read_function(&session_id, sizeof(int)) == -1)
        return -1;

    if(session_id == ALL_TAKEN)
        return -1;

    return 0;
}

int tfs_unmount() {
    int code = TFS_OP_CODE_UNMOUNT;
    char message[1+sizeof(int)];

    memcpy(message, &code, sizeof(char));
    memcpy(message+1, &session_id, sizeof(int));

    if(write_function(&message, 1+sizeof(int)) == -1)
        return -1;

    session_id = ALL_TAKEN;

    if(close_function(fserv) == -1 || close_function(fcli) == -1)
        return -1;

    unlink(client_pipe);

    return 0;
}

int tfs_open(char const *name, int flags) {
    int code = TFS_OP_CODE_OPEN, answer;
    char file_name[NAME_SIZE], message[1+2*sizeof(int)+NAME_SIZE];

    strcpy(file_name, name);

    for(size_t i = strlen(file_name); i < NAME_SIZE; i++)
        file_name[i] = '\0';

    memcpy(message, &code, sizeof(char));
    memcpy(message+1, &session_id, sizeof(int));
    memcpy(message+1+sizeof(int), file_name, NAME_SIZE);
    memcpy(message+1+sizeof(int)+NAME_SIZE, &flags, sizeof(int));

    if(write_function(message, 1+2*sizeof(int)+NAME_SIZE) == -1)
        return -1;

    if(read_function(&answer, sizeof(int)) == -1)
        return -1;

    return answer;
}

int tfs_close(int fhandle) {
    int code = TFS_OP_CODE_CLOSE, answer;
    char message[1+2*sizeof(int)];

    memcpy(message, &code, sizeof(char));
    memcpy(message+1, &session_id, sizeof(int));
    memcpy(message+1+sizeof(int), &fhandle, sizeof(int));

    if(write_function(message, 1+2*sizeof(int)) == -1)
        return -1;

    if(read_function(&answer, sizeof(int)) == -1)
        return -1;

    return answer;
}

ssize_t tfs_write(int fhandle, void const *buffer, size_t len) {
    int code = TFS_OP_CODE_WRITE;
    ssize_t answer;
    char message[sizeof(char)+2*sizeof(int)+sizeof(size_t)+len];

    memcpy(message, &code, sizeof(char));
    memcpy(message+1, &session_id, sizeof(int));
    memcpy(message+1+sizeof(int), &fhandle, sizeof(int));
    memcpy(message+1+2*sizeof(int), &len, sizeof(size_t));
    memcpy(message+1+2*sizeof(int)+sizeof(size_t), buffer, len);
    
    if(write_function(message, sizeof(char)+2*sizeof(int)+sizeof(size_t)+len) == -1)
        return -1;

    if(read_function(&answer, sizeof(ssize_t)) == -1)
        return -1;

    return answer;
}

ssize_t tfs_read(int fhandle, void *buffer, size_t len) {
    int code = TFS_OP_CODE_READ;
    char message[1+2*sizeof(int)+sizeof(size_t)];
    ssize_t answer;

    memcpy(message, &code, sizeof(char));
    memcpy(message+1, &session_id, sizeof(int));
    memcpy(message+1+sizeof(int), &fhandle, sizeof(int));
    memcpy(message+1+2*sizeof(int), &len, sizeof(size_t));

    if(write_function(message, 1+2*sizeof(int)+sizeof(size_t)) == -1)
        return -1;

    if(read_function(&answer, sizeof(ssize_t)) == -1)
        return -1;

    if(answer == -1)
        return -1;

    if(read_function(buffer, (size_t)answer) == -1)
        return -1;
    
    char buffer2[answer];
    memcpy(buffer2, buffer, (size_t)answer);

    return answer;
}

int tfs_shutdown_after_all_closed() {
    int code = TFS_OP_CODE_SHUTDOWN_AFTER_ALL_CLOSED, answer;
    char message[1+sizeof(int)];

    memcpy(message, &code, sizeof(char));
    memcpy(message+1, &session_id, sizeof(int));

    if(write_function(message, 1+sizeof(int)) == -1)
        return -1;

    if(read_function(&answer, sizeof(int)) == -1)
        return -1;

    return answer;
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

int write_function(void *buf, size_t bytes) {
    ssize_t written;
    while((written = write(fserv, buf, bytes)) == -1) {
        if(errno == EINTR)
            continue;
        if(errno == EPIPE)
            return -1;
    }

    if(written < bytes)
        return -1;

    return 0;
}

int read_function(void *buf, size_t bytes) {
    ssize_t rd;
    while((rd = read(fcli, buf, bytes)) == -1) {
        if(errno == EINTR)
            continue;
        if(errno == EPIPE)
            return -1;
    }

    if(rd < bytes)
        read_function((char*)buf+rd, bytes-(size_t)rd);

    return 0;
}