// nanosleep, please
#define _POSIX_C_SOURCE 199309L
// snprintf, please
#define _C99_SOURCE

#include <assert.h>
#include <ctype.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "zlog/src/zlog.h"

#include "raft_c_if.h"

typedef uint32_t* fsm_result_t;

const static uint32_t BUFSIZE = 256;

static zlog_category_t *cat;

static uint32_t   letter_count = 0;

static unsigned        runs = 20;
static unsigned        snapshot_period = 0;
static struct timespec delay = { 1, 0 }; // 1 second
static bool            interactive = false;

void  parse_opts(int argc, char *argv[]);
int   write_snapshot(raft_fsm_snapshot_handle handle, FILE* sink);

void  run_auto();
void  run_interactive();
void  send_command(char* raft_buf);
void  take_snapshot();

// FSM

fsm_result_t update_count(const char *buf, size_t len)
{
    const char * const endp = buf+len;
    while (buf < endp) {
        char c = *buf++;
        if (isalpha(c)) {
            ++letter_count;
        } else if (c == '\0') {
            break;
        }
    }

    uint32_t *result = malloc(sizeof(uint32_t)); // yeah, yeah, it's 4
    assert(result);
    zlog_debug(cat, "Allocated result object @ %p\n", (void*)result);
    *result = letter_count;
    return result;
}

void* FSMApply(uint64_t index, uint64_t term, RaftLogType type, char *cmd, size_t len)
{
    zlog_info(cat, "FSM: applying command (%lu bytes @ %p): %s",
              len, cmd, cmd);
    return update_count(cmd, len);
}

void FSMBeginSnapshot(const char *path, raft_snapshot_req s)
{
    uint32_t *state = malloc(sizeof(uint32_t));
    if (!state) {
        perror("malloc failed");
        exit(1);
    }
    *state = letter_count;

    raft_fsm_take_snapshot(s, (raft_fsm_snapshot_handle) state, &write_snapshot);
}

int FSMRestore(const char *path)
{
    FILE *src = fopen(path, "r");
    if (src) {
        uint32_t val;
        int scanned = fscanf(src, "%u", &val);
        if (fclose(src)) {
            perror("Closing snapshot pipe failed");
            return 1;
        }
        if (scanned == 1) {
            fprintf(stderr, "Read snapshot state: %u\n", val);
            letter_count = val;
            return 0;
        } else {
            fprintf(stderr, "Reading snapshot failed.\n");
            return 1;
        }
    } else {
        perror("Opening snapshot pipe failed");
        return 1;
    }
}

int write_snapshot(raft_fsm_snapshot_handle handle, FILE* sink)
{
    uint32_t *state = (uint32_t*) handle;
    int result;
    int chars = fprintf(sink, "%u\n", *state);
    if (chars < 0) {
        perror("Writing snapshot failed");
        result = 1;
    } else {
        result = 0;
    }
    free(state);
    return result;
}


// Client code

void parse_opts(int argc, char *argv[])
{
    while (true) {
        int c = getopt(argc, argv, "n:s:w:i");
        if (c == -1)
            break;
        switch (c) {
        case 'n':
            runs = strtoul(optarg, NULL, 10);
            break;
        case 's':
            snapshot_period = strtoul(optarg, NULL, 10);
            break;
        case 'w':
            delay.tv_nsec = strtoul(optarg, NULL, 10) * 1000;
            break;
        case 'i':
            interactive = true;
            break;
        }
    }
}

void run_auto()
{
    printf("%u runs, snapshot period %u.\n", runs, snapshot_period);

    for (int i = 1; i <= runs; ++i) {
        char* buf = alloc_raft_buffer(BUFSIZE);
        zlog_debug(cat, "Allocated cmd buffer at %p.", buf);
        snprintf(buf, BUFSIZE, "Raft command #%d", i);
        send_command(buf);

        nanosleep(&delay, NULL);

        if (snapshot_period && i % snapshot_period == 0) {
            take_snapshot();
        }
    }
}

void run_interactive()
{
    for (;;) {
        printf("text, [S]napshot, or [Q]uit?\n");
        char line[256];
        char *res = fgets(line, 256, stdin);
        if (!res) {
            if (ferror(stdin)) {
                zlog_error(cat, "Failed to read line!");
            }
            return;
        }

        char* nl = memchr(line, '\n', 255);
        if (nl)
            *nl = '\0';

        if (strncasecmp("S", line, 256) == 0) {
            take_snapshot();
        } else if (strncasecmp("Q", line, 256) == 0) {
            return;
        } else {
            char* buf = alloc_raft_buffer(BUFSIZE);
            strncpy(buf, line, 255);
            send_command(buf);
        }
    }
}

void send_command(char* buf)
{
    raft_future f = raft_apply_async(buf, BUFSIZE, 0);
    RaftError err = raft_future_wait(f);
    if (!err) {
        fsm_result_t cur_count_p = NULL;
        raft_future_get_ptr(f, (void**)&cur_count_p);
        assert(cur_count_p);
        printf("FSM state: letter count %u.\n", *cur_count_p);
        free(cur_count_p);
    } else {
        fprintf(stderr, "Raft error: %s\n", raft_err_msg(err));
    }
    raft_future_dispose(f);
    free_raft_buffer(buf);
}

void take_snapshot()
{
    printf("Requesting snapshot.\n");
    raft_future sf = raft_snapshot();
    RaftError err = raft_future_wait(sf);
    if (!err) {
        printf("Snapshot complete.\n");
    } else {
        printf("Snapshot failed.\n");
    }
    raft_future_dispose(sf);
}

RaftFSM fsm_def = { &FSMApply, &FSMBeginSnapshot, &FSMRestore };

int main(int argc, char *argv[])
{
    fprintf(stderr, "Raft client starting.\n");
    RaftConfig config;
    if (raft_parse_argv(argc, argv, &config)) {
        fprintf(stderr, "libraft error parsing args!\n");
        exit(1);
    }
    raft_init(&fsm_def, &config);
    cat = zlog_get_category("client");

    parse_opts(argc, argv);

    while (! raft_is_leader()) {
        sleep(1);
    }

    if (interactive) {
        run_interactive();
    } else {
        run_auto();
    }

    raft_future sf = raft_shutdown();
    raft_future_wait(sf);
    printf("Raft is shut down.\n");
    raft_cleanup();

    return 0;
}
