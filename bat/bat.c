//  bat.cpp
//  Created by Victor Grishchenko on 09.01.15.
//  Copyright (c) 2015 Victor Grishchenko. All rights reserved.
const char help[] =
"BAT - Blackbox Automated Testing\n\
The tool does replay-and-compare blackbox testing with some twists. \
Input and output are generally supposed to be line-based. \
But in fact, BAT works in terms of input/output blocks. \
An input block is read from the script file and fed to the program; \
the resulting output is compared with the expected one. \
The output is not necessarily static as it may include timestamps, \
variable whitespace etc. For that purpose, the user may define PCRE \
regular expressions to collapse or unify those variable pieces. \
The tool is written in C++ with various optimizations to be used for \
load tests as well as unit and black box spec compliance tests.\n\n";

#include <unistd.h>
#include <stdio.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <math.h>
#include <sys/stat.h>
#include <sys/select.h>

#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>

int diff_flag = 0,
verbose_flag = 1;

#define VERBOSE if (verbose_flag) printf

const size_t MAX_BLOCK_SIZE = (1<<20);
const size_t MAX_LINE_SIZE = (1<<19);

pcre2_code **collapsibles = NULL;
char **collapsible_names = NULL;
int collapsible_count = 0;

int parse_collapsible_expressions (FILE* file) {
    size_t collapsible_array_size = 128;
    size_t ptrsize = sizeof(pcre2_code *);
    collapsibles = malloc(ptrsize*collapsible_array_size);
    unsigned char* pattern = NULL;
    size_t plength = 0, psize = 0;
    int errorcode = 0;
    size_t erroroffset;
    
    while ( 0 < (plength=getline((char**)&pattern,&psize,file)) ) {
        if (plength<=1) continue; // empty line;
        // PARSE:  ^regex \s string
        collapsibles[collapsible_count] = pcre2_compile (
               pattern,
               plength-1,
               0,
               &errorcode,
               &erroroffset,
               NULL
        );
        if (errorcode) {
            PCRE2_UCHAR errmsg[1024];
            pcre2_get_error_message(errorcode, errmsg, 1024);
            fprintf(stderr,"collapsible pcre compile error: %s", errmsg);
            break;
        }
        if (collapsible_count++ == collapsible_array_size) {
            collapsible_array_size<<=1;
            collapsibles = realloc(collapsibles, ptrsize);
        }
    }
    
    free(pattern);
    return collapsible_count;
}


struct buf_t { // FIXME iovec
    char*  buf;
    size_t len;
};

const suseconds_t SURE_WAIT = 100000; // 100ms

size_t MEM_SIZE = 1<<20;

char  * mem;
char *mem_head, *mem_tail;

void free_bufs () {
    mem_head = mem;
}

struct timeval ARE_YOU_SURE_WAIT = {1,0}; // 1sec
struct timeval ANYTHING_ELSE_WAIT = {0,100000}; // .1sec


/** reads a piece of exchange based on timing...
  * returns bytes read or 0 for a timeout or -1 for an error */
ssize_t read_buf (int file, struct buf_t* buf, suseconds_t wait, struct buf_t *ref) {
    buf->buf = mem_head;
    buf->len = 0;
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(file,&fds);
    struct timeval timeout = ARE_YOU_SURE_WAIT;
    int s;

    while ( 0 < (s=select(file+1, &fds, NULL, NULL, &timeout)) ) {
        ssize_t r = read(file,
                         buf->buf + buf->len,
                         (size_t)(mem_tail-mem_head)-buf->len);
        if (r>0) {
            buf->len += r;
            timeout = ANYTHING_ELSE_WAIT;
        } else if (r==0) { // eof
            break;
        } else {
            perror("read error");
            return -1;
        }
    }
    if (s<0) {
        perror("select error");
        return -1;
    }
    mem_head = buf->buf + buf->len;
    return s > 0 ? buf->len : 0;
}

void sigpipe_handler (int signum) {
    fprintf(stderr,"pipe closed");
}

ssize_t write_buf (int file, struct buf_t buf) {
    ssize_t ret = write(file, buf.buf, buf.len); // FIXME part
    return ret;
}

#define MAX_READ 4096
char stash_page[MAX_READ];
struct buf_t stash_buf = {stash_page,0};

char SEPARATOR_REQ[64] = "~>";
char SEPARATOR_RES[64] = "<~";


int read_till_delim (int file, char* delim, struct buf_t* buf, struct buf_t* stash) {
    // start with the stash
    memcpy(mem_head,stash->buf,stash->len);
    ssize_t len=stash->len, r=0;
    char* loc=NULL;
    while (!(loc=strnstr(mem_head, delim, len))) {
        r = read(file, mem_head+len, MAX_READ);
        if (r>0) {
            len += r;
        } else if (r==0) { // EOF
            loc = mem_head + len;
            break;
        } else {
            perror("read fail");
            exit(-8);
        }
    }
    size_t dlen = strlen(delim);
    buf->buf = mem_head + dlen;
    buf->len = loc - mem_head - dlen;
    stash->len = len - (loc - mem_head);
    memcpy(stash->buf, loc, stash->len);
    mem_head = loc;
    return r>0 || stash->len; // TODO EOF
}

ssize_t read_cycle(int file, struct buf_t* in_ref, struct buf_t* out_ref) {
    
    // TODO EOF
    read_till_delim(file, SEPARATOR_RES, in_ref, &stash_buf);
    int eoff = read_till_delim(file, SEPARATOR_REQ, out_ref, &stash_buf);
    
    return eoff;
}

ssize_t write_cycle(int file, struct buf_t req, struct buf_t res) {
    write(file, SEPARATOR_REQ, strlen(SEPARATOR_REQ));
    write(file, req.buf, req.len); // FIXME partial
    write(file, SEPARATOR_RES, strlen(SEPARATOR_RES));
    write(file, res.buf, res.len);
    return 0;
}

struct buf_t collapse (struct buf_t orig) {
    struct buf_t buf = orig;
    size_t len = 0;
    for(int i=0; i<collapsible_count; i++) {
        int subs = pcre2_substitute
                         (collapsibles[i],
                          (PCRE2_SPTR)buf.buf,
                          buf.len,
                          (PCRE2_SIZE)0,
                          0,
                          NULL,
                          NULL,
                          (PCRE2_SPTR)collapsible_names[i],
                          PCRE2_ZERO_TERMINATED,
                          (PCRE2_UCHAR*)mem_head,
                          &len
                         );
        if (subs) {
            buf.buf = mem_head;
            buf.len = len;
            mem_head += len;
        }
    }
    return buf;
}


int compare_bufs (struct buf_t ref, struct buf_t fact) {
    struct buf_t ref_c = collapsible_count ? collapse(ref) : ref;
    struct buf_t fact_c = collapsible_count ? collapse(fact) : fact;
    if (ref_c.len!=fact_c.len) return -1;
    return memcmp(ref_c.buf,fact_c.buf,ref_c.len);
}


int open_process (const char* command, int fildes[2]) {
    char _command[1024];
    strcpy(_command, command);
    char* prog = strtok(_command," ");
    char* args[10];
    VERBOSE("starting %s\n",prog);
    for(int i=0; i<10 && (args[i]=strtok(NULL," ")); i++) {
        VERBOSE("\t%s\n",args[i]);
    }
    
    int pipe_in[2], pipe_out[2];
    if (pipe(pipe_in)==-1 || pipe(pipe_out)==-1) {
        perror("pipe open fails");
        exit(1);
    }
    
    pid_t pid = fork();
    if (pid == 0) { // child
        while ((dup2(pipe_in[1], STDOUT_FILENO) == -1) && (errno==EINTR));
        while ((dup2(pipe_out[0], STDIN_FILENO) == -1) && (errno==EINTR));
        close(pipe_in[1]);
        close(pipe_in[0]);
        close(pipe_out[1]);
        close(pipe_out[0]);
        extern char **environ;
        environ = NULL;
        
        execvp(prog, args); // execvp
        
        perror("exec failed");
        _exit(1);
    }
    close(pipe_in[1]);
    close(pipe_out[0]);
    fildes[0] = pipe_in[0];
    fildes[1] = pipe_out[1];
    return pid;
}


int main(int argc, char * const * argv) {
    
    int c;
    extern char *optarg;
    extern int errno;
    int client[] = {0,0}, server[] = {0,0};
    int script = 0, record = 0;
    FILE* clp_file = NULL;
    float timef;
    long m;
    struct timeval timetv = {0,0};
    signal (SIGPIPE, sigpipe_handler);
    
    while ((c = getopt(argc, argv, "v:S:C:s:Rr:t:T:m:d")) != -1) {
        switch(c) {
            case 'S': // server
                if ( -1 == open_process(optarg,server) )  {
                    perror("fork failed for the server");
                    return -3;
                } else {
                    VERBOSE("started %s as a server\n",optarg);
                }
                break;
            case 'C': // client TODO: target is a TCP socket (has :)
                if ( -1 == open_process(optarg,client) ){
                    perror("fork failed for the client");
                    return -2;
                } else {
                    VERBOSE("started %s as a client\n",optarg);
                }
                break;
            case 'R':
                record = 1;
                break;
            case 'r': // record a session
                record = open(optarg,
                              O_WRONLY|O_CREAT|O_TRUNC,
                              S_IRUSR|S_IWUSR|S_IRGRP);
                if ( -1 == record ) {
                    perror("can't open record");
                    return -7;
                } else {
                    VERBOSE("session is recorded to %s\n",
                            optarg?optarg:"stdout");
                }
                break;
            case 's': // script (a file of requests +expected responses)
                if ( -1 == (script = open(optarg,O_RDONLY)) ) {
                    perror("can't open script");
                    return -6;
                }
                break;
            case 'T':
            case 't':
                if (1==sscanf(optarg, "%f", &timef)) {
                    timetv.tv_usec =
                        (suseconds_t) ((int)(timef*1000000)%1000000);
                    timetv.tv_sec =
                        (time_t) lrintf(timef);
                } else {
                    fprintf(stderr,"expected time format: sec.usec");
                }
                if (c=='t') {
                    ANYTHING_ELSE_WAIT = timetv;
                } else {
                    ARE_YOU_SURE_WAIT = timetv;
                }
                break;
            case 'm':
                if (1==sscanf(optarg, "%ld", &m)) {
                    MEM_SIZE = m;
                } else {
                    fprintf(stderr,"m: max memory allocation in bytes");
                }
                break;
            case 'c': // collapsibles
                if ( ! (clp_file = fopen(optarg,O_RDONLY)) ) {
                    fprintf(stderr, "can't open collapsibles: %s", strerror(errno));
                    return -1;
                } else {
                    parse_collapsible_expressions(clp_file);
                    fclose(clp_file);
                }
                break;
            case 'd': // run diff
                diff_flag = 1;
                break;
            case 'v':
                verbose_flag = 1;
        }
    }
    
    mem = malloc(MEM_SIZE);
    mem_head = mem;
    mem_tail = mem + MEM_SIZE;
    
    struct buf_t in_ref = {NULL,0}, out_ref = {NULL,0};
    struct buf_t in_buf = {NULL,0}, out_buf = {NULL,0};
    int scr=0;
    
    if (!script && !(*client&&*server)) {
        // what?!
        return -2;
    }
    
    do {
        if (script) {
            scr = read_cycle(script, &in_ref, &out_ref);
        }
        if (*client) {
            read_buf(client[0], &in_buf, SURE_WAIT, NULL); // TODO stop if OK
            if (script) {
                compare_bufs(in_ref,in_buf);
            }
        }
        if (*server){
            if ( 0 > write_buf(server[1], *client ? in_buf : in_ref) ) {
                perror("server write fails");
                return -5;
            }
            read_buf(server[0], &out_buf, SURE_WAIT, NULL);
            if (script) {
                if (0==compare_bufs(out_ref,out_buf)) {
                    printf("OK\n");
                } else {
                    printf("FAIL\n");
                }
            }
        }
        if (*client) {
            write_buf(client[1], *server ? out_buf : out_ref);
        }
        if (record && (in_buf.len || out_buf.len)) {
            write_cycle( record,
                        *client ? in_buf : in_ref,
                        *server ? out_buf : out_ref );
        }
        free_bufs();
    } while ( (script&&scr>0) || (!script && (in_buf.len || out_buf.len)) );
    
    
    if (client[0]) close(client[0]);
    if (client[1]) close(client[1]);
    if (server[0]) close(server[0]);
    if (server[1]) close(server[1]);
    if (script) close(script);
    if (record) close(record);
    
    return 0;
}
