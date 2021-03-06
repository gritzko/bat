//  bat.cpp
//  Created by Victor Grishchenko on 09.01.15.
//  Copyright (c) 2015 Victor Grishchenko. All rights reserved.
const char help[] =
"BAT - Blackbox Automated Testing\n\
The tool does replay-and-compare blackbox testing with some twists. \n\
Input and output works in terms of input/output blocks. For example, \n\
an input block is read from the script file and fed to the program; \n\
the resulting output is compared with the expected one. Or, two programs \n\
are run together, their exchange recorded for later replay. As bat\n\
has no knowledge of protocol semantics, I/O blocks are delay based, e.g.\n\
10ms silence means end-of-block. Thus, recording may be slow. Replaying, \n\
on the other hand, does not need delays (mostly), so it is fast. \n\
Program's output is not necessarily static as it may include timestamps, \n\
variable whitespace etc. For that purpose, the user may define PCRE \n\
regular expressions to collapse or unify those variable pieces. \n\
The tool is written in C with various optimizations to be used for \n\
load tests as well as unit and black box spec compliance tests.\n\n\
Options:\n\
-h help\n\
-S server process (command)\n\
-C client process (command)\n\
-r record the session to stdout\n\
-R record the session to a file (file name)\n\
-s script (likely, a past record to replay to a server or a client)\n\
-t time to wait after the process gives a correct response; maybe\n\
   it will add something that will make it incorrect (sec.usec)\n\
-T time to wait after the process gives an incorrect response; maybe\n\
   it will add something that will make it correct (sec.usec)\n\
-m size of memory buffers to allocate (bytes, default 1 megabyte)\n\
-c collapsible patterns (file format: NAME-SPACE-PCRE-NEWLINE, repeat)\n\
\n";

#include <unistd.h>
#include <stdio.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <math.h>
#include <sys/stat.h>
#include <sys/select.h>
#include <signal.h>

#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>

int diff_flag = 0,
    verbose_flag = 0,
    trace_flag = 0;

#define VERBOSE if (verbose_flag) printf
#define TRACE if (trace_flag) printf

const size_t MAX_BLOCK_SIZE = (1<<20);
const size_t MAX_LINE_SIZE = (1<<19);

#define MAX_CLP_RULES 32
pcre2_code *collapsibles[MAX_CLP_RULES];
char *collapsible_names[MAX_CLP_RULES];
int collapsible_count = 0;

int parse_collapsible_expressions (FILE* file) {
    char *clp_rule = NULL;
    ssize_t plength = 0;
    size_t rsize = 0;
    int errorcode = 0;
    size_t erroroffset;

    while ( 0 < (plength=getline((char**)&clp_rule,&rsize,file)) ) {
        if (plength<=1) continue; // empty line;
        TRACE("collapsible: %s\n", clp_rule);
        char *end = strstr(clp_rule,"\n");
        if (end) *end = 0;
        char *pattern = strstr(clp_rule," ");
        if (!pattern) {
            fprintf(stderr,"invalid pattern: %s\n",clp_rule);
            continue;
        }
        for (;*pattern==' '; *pattern=0, pattern++);
        collapsible_names[collapsible_count] = clp_rule;

         pcre2_code *re = pcre2_compile (
               (PCRE2_SPTR)pattern,
               end-pattern,
               0,
               &errorcode,
               &erroroffset,
               NULL
        );

        if (re) {
            collapsibles[collapsible_count] = re;
        } else {
            PCRE2_UCHAR errmsg[1024];
            pcre2_get_error_message(errorcode, errmsg, 1024);
            fprintf(stderr,"collapsible pcre compile error: %s", errmsg);
        }
        if (++collapsible_count == MAX_CLP_RULES) {
            fprintf(stderr,"%i rules max\n",MAX_CLP_RULES);
            break;
        }
        clp_rule = NULL;
    }
    VERBOSE("parsed %i expressions\n", collapsible_count);

    return collapsible_count;
}


struct buf_t {
    char*  buf;
    size_t len;
};

const suseconds_t SURE_WAIT = 100000; // 100ms

// TODO flashbuf_t !
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
    mem_head[stash->len] = 0;
    ssize_t len=stash->len, r=0;
    char* loc=NULL;
    while (!(loc=strstr(mem_head, delim))) {
        r = read(file, mem_head+len, MAX_READ);
        if (r==0) { // EOF
            loc = mem_head + len;
            break;
        } else if (r<0) {
            perror("read fail");
            exit(-8);
        }
        len += r;
        mem_head[len] = 0;
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
    int i;
    for(i=0; i<collapsible_count; i++) {
        size_t len = mem_tail - mem_head; // TODO flashbuf_t
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
        if (subs>0) {
            buf.buf = mem_head;
            buf.len = len;
            mem_head += len;
        } else if (subs<0) {
            PCRE2_UCHAR errmsg[1024];
            pcre2_get_error_message(subs, errmsg, 1024);
            fprintf(stderr,"collapsible pcre compile error: %s", errmsg);
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
    char* args[10];
    char* prog = args[0] = strtok(_command," ");
    int i;
    for(i=1; i<10 && (args[i]=strtok(NULL," ")); i++);

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

    int c, opts=0;
    extern char *optarg;
    extern int errno;
    int client[] = {0,0}, server[] = {0,0};
    int script = 0, record = 0;
    FILE* clp_file = NULL;
    float timef;
    long m;
    struct timeval timetv = {0,0};
    signal (SIGPIPE, sigpipe_handler);
    int fails = 0, tests = 0;

    while ((c = getopt(argc, argv, "hvVdS:C:s:Rr:t:T:m:c:")) != -1) {
        opts++;
        switch(c) {
            case 'h':
                printf("%s",help);
                return 0;
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
                VERBOSE("recording to stdout\n");
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
                } else {
                    VERBOSE("open script %s\n",optarg);
                }
                break;
            case 'T':
            case 't':
                if (1==sscanf(optarg, "%f", &timef)) {
                    timetv.tv_usec =
                        (suseconds_t) ((int)(timef*1000000)%1000000);
                    timetv.tv_sec =
                        (time_t) (int) timef;
                } else {
                    fprintf(stderr,"expected time format: sec.usec");
                }
                if (c=='t') {
                    ANYTHING_ELSE_WAIT = timetv;
                } else { // T
                    ARE_YOU_SURE_WAIT = timetv;
                }
                break;
            case 'm':
                if (1==sscanf(optarg, "%ld", &m)) {
                    MEM_SIZE = m;
                    VERBOSE("using %ld bytes of RAM\n",m);
                } else {
                    fprintf(stderr,"m: max memory allocation in bytes");
                }
                break;
            case 'c': // collapsibles
                if ( ! (clp_file = fopen(optarg,"r")) ) {
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
                break;
            case 'V':
                trace_flag = 1;
                break;
        }
    }
    if (!opts) {
        printf("%s",help);
        return 0;
    }

    mem = malloc(MEM_SIZE);
    mem_head = mem;
    mem_tail = mem + MEM_SIZE;

    struct buf_t in_ref = {NULL,0}, out_ref = {NULL,0};
    struct buf_t in_buf = {NULL,0}, out_buf = {NULL,0};
    ssize_t scr=0;

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
                tests++;
                if (0==compare_bufs(out_ref,out_buf)) {
                    printf("OK\n");
                } else {
                    fails++;
                    printf("FAIL\n");
                    if (diff_flag) {
                        fprintf(stderr, "EXPECTED:\n");
                        write_buf(2, out_ref);
                        fprintf(stderr, "RECEIVED:\n");
                        write_buf(2, out_buf);
                    }
                }
            }
        }
        if (*client) {
            write_buf(client[1], *server ? out_buf : out_ref);
        }
        if (record && (script || in_buf.len || out_buf.len)) {
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

    return fails;
}
