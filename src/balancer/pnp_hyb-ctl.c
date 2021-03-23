#include "pnp.h"
#include "pcm-pnp.h"

#include <sys/socket.h>
#include <sys/select.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <linux/netlink.h>

#include <pthread.h>

#include <numaif.h>
#include <numa.h>
#include <math.h>
#include <syscall.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <errno.h>
#include <emmintrin.h>

int netlink_fd;

long page_size; // in kB

struct sockaddr_nl src_addr, dst_addr;
struct nlmsghdr *nlmh_out;//, *nlmh_in;

char *buffer;
int buf_size;

addr_info_t *candidates;

struct iovec iov_out, iov_in;
struct msghdr msg_out, msg_in;

volatile int exit_sig = 0;
volatile int nvramWrChk_act = 1;
volatile int thresh_act = 1;
volatile int balance_act = 1;

// In microseconds
int memcheck_interval = MEMCHECK_INTERVAL * 1000;
int clearDirty_interval = CLEAR_DELAY * 1000;
int nvramWrChk_interval = NVRAMWRCHK_INTERVAL * 1000;

pthread_t stdin_thread, socket_thread, memcheck_thread, nvramWrChk_thread;
pthread_mutex_t comm_lock, placement_lock;



/*
-------------------------------------------------------------------------------

NETLINK CONFIGURATION

-------------------------------------------------------------------------------
*/


void configure_netlink_addr() {
    /* Source and destination addresses config */
    // source address
    memset(&src_addr, 0, sizeof(src_addr));
    src_addr.nl_family = AF_NETLINK;
    src_addr.nl_pid = getpid();
    src_addr.nl_groups = 0; // unicast

    // destination address
    memset(&dst_addr, 0, sizeof(dst_addr));
    dst_addr.nl_family = AF_NETLINK;
    dst_addr.nl_pid = 0; // kernel
    dst_addr.nl_groups = 0; // unicast
}

void configure_netlink_outbound() {

    /* netlink message header config */
    nlmh_out->nlmsg_len = NLMSG_SPACE(MAX_PAYLOAD);
    nlmh_out->nlmsg_pid = getpid();
    nlmh_out->nlmsg_flags = 0;

    /* IO vector out config */
    iov_out.iov_base = (void *) nlmh_out;
    iov_out.iov_len = nlmh_out->nlmsg_len;

    /* message header outconfig */
    msg_out.msg_name = (void *) &dst_addr;
    msg_out.msg_namelen = sizeof(dst_addr);
    msg_out.msg_iov = &iov_out;
    msg_out.msg_iovlen = 1;
}

void configure_netlink_inbound() {

    /* IO vector in config */
    iov_in.iov_base = (void *) buffer;
    iov_in.iov_len = buf_size;

    /* message header in config */
    msg_in.msg_name = (void *) &dst_addr;
    msg_in.msg_namelen = sizeof(dst_addr);
    msg_in.msg_iov = &iov_in;
    msg_in.msg_iovlen = 1;
}



/*
-------------------------------------------------------------------------------

HELPER FUNCTIONS

-------------------------------------------------------------------------------
*/

int check_memdata(memdata_t *md) {
    if ((md == NULL) || !BETWEEN(md->sys_dramReads, 0, DRAM_BW_MAX) || !BETWEEN(md->sys_dramWrites, 0, DRAM_BW_MAX)
            || !BETWEEN(md->sys_pmmReads, 0, NVRAM_BW_MAX) || !BETWEEN(md->sys_pmmWrites, 0, NVRAM_BW_MAX)
            || !BETWEEN(md->sys_pmmAppBW, 0, NVRAM_BW_MAX) || !BETWEEN(md->sys_pmmMemBW, 0, NVRAM_BW_MAX)) {
        return 0;
    }

    return 1;
}

memdata_t *read_memdata() {
    memdata_t *md = malloc(sizeof(memdata_t));

    FILE * in_file = fopen(PCM_FILE_NAME, "r");
    if (in_file == NULL) {
        fprintf(stderr, "Error opening memdata file.\n");
        return md;
    }
    if (fread(md, sizeof(memdata_t), 1, in_file) != 1) {
        fprintf(stderr, "Error reading memdata from file.\n");
    }

    fclose(in_file);

    return md;
}

time_t get_memdata_mtime() {
    struct stat st;
    if (stat(PCM_FILE_NAME, &st) != -1) {
        return st.st_mtime;
    }
    return 0;
}


long long free_space_node(int node, long long *sz) {
    long long node_fr = 0;
    *sz = numa_node_size64(node, &node_fr);
    return node_fr;
}

long long free_space_tot_bytes(int mode, long long *sz) {

    long long total_node_sz = 0;
    long long total_node_fr = 0;

    if (mode == DRAM_MODE) {
        for (int i=0; i < n_dram_nodes; i++) {
            long long node_sz = 0;
            total_node_fr += free_space_node(DRAM_NODES[i], &node_sz);
            total_node_sz += node_sz;
        }
    }
    else {
        for (int i=0; i < n_nvram_nodes; i++) {
            long long node_sz = 0;
            total_node_fr += free_space_node(NVRAM_NODES[i], &node_sz);
            total_node_sz += node_sz;
        }
    }

    *sz = total_node_sz;
    return total_node_fr;
}

float free_space_per(int node) {
    long long sz = 0;
    long long fr = free_space_node(node, &sz);
    return 1.0 * (sz - fr) / sz;
}

float free_space_tot_per(int mode, long long *sz) {
    long long fr = free_space_tot_bytes(mode, sz);
    return 1.0 * (*sz - fr) / *sz;
}

int free_space_pages(int node) {
    long long sz = 0;
    return free_space_node(node, &sz) / page_size;
}

int free_space_tot_pages(int mode) {
    long long sz = 0;
    return free_space_tot_bytes(mode, &sz) / page_size;
}



/*
-------------------------------------------------------------------------------

MIGRATION FUNCTIONS

-------------------------------------------------------------------------------
*/


int do_migration(int mode, int n_found) {
    void **addr = malloc(sizeof(unsigned long) * n_found);
    int *dest_nodes = malloc(sizeof(int) * n_found);
    int *status = malloc(sizeof(int) * n_found);

    const int *node_list;
    int size;

    if (mode == DRAM_MODE) {
        node_list = NVRAM_NODES;
        size = n_nvram_nodes;
    }
    else {
        node_list = DRAM_NODES;
        size = n_dram_nodes;
    }

    for (int i=0; i< n_found; i++) {
        status[i] = -123;
    }

    int n_processed = 0;
    for (int i=0; (i < size) && (n_processed < n_found); i++) {
        int curr_node = node_list[i];

        int n_avail_pages = free_space_pages(curr_node);

        long j=0;
        for (; (j < n_avail_pages) && (n_processed+j < n_found); j++) {
            addr[n_processed+j] = (void *) candidates[n_processed+j].addr;
            dest_nodes[n_processed+j] = curr_node;
        }

        n_processed += j;
    }
    int n_migrated, i;
    int e = 0; // counts failed migrations

    for (n_migrated=0, i=0; n_migrated < n_processed; n_migrated+=i) {
        int curr_pid;
        curr_pid=candidates[n_migrated].pid_retval;

        for (i=1; (candidates[n_migrated+i].pid_retval == curr_pid) && (n_migrated+i < n_processed); i++);

        void **addr_displacement = addr + n_migrated;
        int *dest_nodes_displacement = dest_nodes + n_migrated;
        if (move_pages(curr_pid, (unsigned long) i, addr_displacement, dest_nodes_displacement, status, 0)) {
            // Migrate all and output addresses that could not migrate
            for (int j=0; j < i; j++) {
                if (move_pages(curr_pid, 1, addr_displacement + j, dest_nodes_displacement + j, status, 0)) {
                    printf("Error migrating addr: %ld, pid: %d\n", (unsigned long) *(addr_displacement + j), curr_pid);
                    e++;
                }
                //else if (mode == DRAM_MODE) {
                //    _mm_clflush(addr_displacement + j);
                //}
            }
        }
        //else if (mode == DRAM_MODE) {
         //   for (int j=0; j < i; j++) {
          //      _mm_clflush(addr_displacement + j);
           // }
        //}
    }

    free(addr);
    free(dest_nodes);
    free(status);
    return n_migrated - e;
}

/* int do_switch(int dram_found, int nvram_found) {
    void **addr_dram = malloc(sizeof(unsigned long) * dram_found);
    int *dest_nodes_dram = malloc(sizeof(int) * dram_found);
    void **addr_nvram = malloc(sizeof(unsigned long) * nvram_found);
    int *dest_nodes_nvram = malloc(sizeof(int) * nvram_found);
    int max_found = fmax(dram_found, nvram_found);
    int *status = malloc(sizeof(int) * max_found);

    for (int i=0; i< max_found; i++) {
        status[i] = -123;
    }

    int dram_migrated = 0;
    int nvram_migrated = 0;
    int dram_e = 0; // counts failed migrations
    int nvram_e = 0; // counts failed migrations

    while ((dram_migrated < dram_found) || (nvram_migrated < nvram_found)) {
        // DRAM -> NVRAM
        int old_n_processed = dram_migrated + dram_e;
        int dram_processed = old_n_processed;

        for (int i=0; (i < n_nvram_nodes) && (dram_processed < dram_found); i++) {
            int curr_node = NVRAM_NODES[i];

            long long node_fr = 0;
            numa_node_size64(curr_node, &node_fr);
            int n_avail_pages = node_fr/page_size;

            int j=0;
            for (; (j < n_avail_pages) && (j+dram_processed < dram_found); j++) {
                addr_dram[dram_processed+j] = (void *) candidates[dram_found+1+j].addr;
                dest_nodes_nvram[dram_processed+j] = curr_node;
            }

            dram_processed += j;
        }
        if (old_n_processed < dram_processed) {
            // Send processed pages to NVRAM
            int n_migrated, i;

            for (n_migrated=0, i=0; n_migrated < dram_processed; n_migrated+=i) {
                int curr_pid;
                curr_pid = candidates[dram_found+1+n_migrated].pid_retval;

                for (i=1; (candidates[dram_found+1+n_migrated+i].pid_retval == curr_pid) && (n_migrated+i < dram_processed); i++);
                void **addr_displacement = addr_dram + n_migrated;
                int *dest_nodes_displacement = dest_nodes_nvram + n_migrated;
                if (numa_move_pages(curr_pid, (unsigned long) i, addr_displacement, dest_nodes_displacement, status, 0)) {
                    // Migrate all and output addresses that could not migrate
                    for (int j=0; j < i; j++) {
                        if (numa_move_pages(curr_pid, 1, addr_displacement + j, dest_nodes_displacement + j, status, 0)) {
                            printf("Error migrating DRAM/MEM addr: %ld, pid: %d\n", (unsigned long) *(addr_displacement + j), curr_pid);
                            dram_e++;
                        }
                    }
                }
            }
        }

        dram_migrated = dram_processed - dram_e;

        // NVRAM -> DRAM
        old_n_processed = nvram_migrated + nvram_e;
        int nvram_processed = old_n_processed;

        for (int i=0; (i < n_dram_nodes) && (nvram_processed < nvram_found); i++) {
            int curr_node = DRAM_NODES[i];

            long long node_fr = 0;
            numa_node_size64(curr_node, &node_fr);
            int n_avail_pages = node_fr/page_size;

            int j=0;
            for (; (j < n_avail_pages) && (j+nvram_processed < nvram_found); j++) {
                addr_nvram[nvram_processed+j] = (void *) candidates[nvram_processed+j].addr;
                dest_nodes_dram[nvram_processed+j] = curr_node;
            }

            nvram_processed += j;
        }

        if (old_n_processed < nvram_processed) {
            // Send processed pages to DRAM
            int n_migrated, i;

            for (n_migrated=0, i=0; n_migrated < nvram_processed; n_migrated+=i) {
                int curr_pid;
                curr_pid=candidates[n_migrated].pid_retval;

                for (i=1; (candidates[n_migrated+i].pid_retval == curr_pid) && (n_migrated+i < nvram_processed); i++);
                void **addr_displacement = addr_nvram + n_migrated;
                int *dest_nodes_displacement = dest_nodes_dram + n_migrated;
                if (numa_move_pages(curr_pid, (unsigned long) i, addr_displacement, dest_nodes_displacement, status, 0)) {
                    // Migrate all and output addresses that could not migrate
                    for (int j=0; j < i; j++) {
                        if (numa_move_pages(curr_pid, 1, addr_displacement + j, dest_nodes_displacement + j, status, 0)) {
                            printf("Error migrating NVRAM addr: %ld, pid: %d\n", (unsigned long) *(addr_displacement + j), curr_pid);
                            nvram_e++;
                        }
                    }
                }
            }
        }

        nvram_migrated = nvram_processed - nvram_e;
    }

    free(addr_dram);
    free(addr_nvram);
    free(dest_nodes_dram);
    free(dest_nodes_nvram);
    free(status);

    return dram_migrated + nvram_migrated;
} */



/*
-------------------------------------------------------------------------------

REQUEST PROCESSING FUNCTIONS

-------------------------------------------------------------------------------
*/


int send_req(req_t req, addr_info_t **out) {

    pthread_mutex_lock(&comm_lock);

    memset(NLMSG_DATA(nlmh_out), 0, MAX_PAYLOAD);
    memcpy(NLMSG_DATA(nlmh_out), &req, sizeof(req));
    sendmsg(netlink_fd, &msg_out, 0);

    //configure_netlink_inbound();
    memset(buffer, 0, buf_size);
    int len = recvmsg(netlink_fd, &msg_in, 0);

    addr_info_t *curr_pointer = *out;
    int i = 0;
    struct nlmsghdr * curr_nlmh;
    for (curr_nlmh = (struct nlmsghdr *) buffer; NLMSG_OK(curr_nlmh, len); curr_nlmh = NLMSG_NEXT(curr_nlmh, len)) {
        if (curr_nlmh->nlmsg_type == NLMSG_ERROR) {
            pthread_mutex_unlock(&comm_lock);
            return 0;
        }
        int payload_len = NLMSG_PAYLOAD(curr_nlmh, 0);
        memcpy(curr_pointer, (addr_info_t *) NLMSG_DATA(curr_nlmh), payload_len);
        curr_pointer += payload_len/sizeof(addr_info_t);
        i++;

    }
    pthread_mutex_unlock(&comm_lock);
    return 1;
}

int send_bind(int pid) {
    req_t req;
    addr_info_t *op_retval = malloc(sizeof(addr_info_t));

    req.op_code = BIND_OP;
    req.pid_n = pid;

    send_req(req, &op_retval);
    if (op_retval->pid_retval == 0) {
        free(op_retval);
        return 1;
    }
    free(op_retval);
    return 0;
}

int send_unbind(int pid) {
    req_t req;
    addr_info_t *op_retval = malloc(sizeof(addr_info_t));

    req.op_code = UNBIND_OP;
    req.pid_n = pid;

    send_req(req, &op_retval);
    if (op_retval->pid_retval == 0) {
        free(op_retval);
        return 1;
    }
    free(op_retval);
    return 0;
}

int send_find(int n_pages, int mode) {
    req_t req;

    req.op_code = FIND_OP;
    req.pid_n = n_pages;
    req.mode = mode;

    send_req(req, &candidates);

    int n_found=-1;

    while (candidates[++n_found].pid_retval > 0);

    if (n_found == 0) {
        return 0;
    }
    switch (mode) {
        case DRAM_MODE:
            return do_migration(DRAM_MODE, n_found);
            break;
        case NVRAM_MODE:
            return do_migration(NVRAM_MODE, n_found);
            break;
        case NVRAM_WRITE_MODE:
            return do_migration(NVRAM_WRITE_MODE, n_found);
            break;
        case BALANCE_DRAM_MODE:
        case BALANCE_NVRAM_MODE: ; // Empty statement because labels must be followed by a statement (not declaration)
            int bal_mode = DRAM_MODE;
            if(mode == BALANCE_NVRAM_MODE) {
                bal_mode = NVRAM_MODE;
            }
            return do_migration(bal_mode, n_found);
            break;
    }
    return 0;
}



/*
-------------------------------------------------------------------------------

PLACEMENT FUNCTIONS

-------------------------------------------------------------------------------
*/


void *memcheck_placement(void *args) {
    long long dram_sz = 0;
    long long nvram_sz = 0;
    float dram_usage;
    float nvram_usage;
    int n_pages;
    time_t prev_memdata_lmod = 0;

    while (!exit_sig) {
        int any_full = 0; // set if the threshold component is active and finds any mode full
        int n_migrated = 0;
        int sleep_interval = memcheck_interval;

        if (thresh_act) {
            dram_usage = free_space_tot_per(DRAM_MODE, &dram_sz);
            nvram_usage = free_space_tot_per(NVRAM_MODE, &nvram_sz);
            printf("Current DRAM Usage: %0.2f%%\n", dram_usage*100);
            printf("Current NVRAM Usage: %0.2f%%\n", nvram_usage*100);

            if ((dram_usage > DRAM_LIMIT) && (nvram_usage < NVRAM_TARGET)) {
                long long n_bytes = fmin((dram_usage - DRAM_TARGET) * dram_sz,
                                    (NVRAM_TARGET - nvram_usage) * nvram_sz);
                n_pages = n_bytes / page_size;
                n_pages = fmin(n_pages, MAX_N_FIND);
                pthread_mutex_lock(&placement_lock);
                n_migrated = send_find(n_pages, DRAM_MODE);
                pthread_mutex_unlock(&placement_lock);
                if (n_migrated > 0) {
                    printf("DRAM->NVRAM: Migrated %d out of %d pages.\n", n_migrated, n_pages);
                }
            }
            else if ((nvram_usage > NVRAM_LIMIT) && (dram_usage < DRAM_TARGET)) {
                long long n_bytes = fmin((nvram_usage - NVRAM_TARGET) * nvram_sz,
                                    (DRAM_TARGET - dram_usage) * dram_sz);
                n_pages = n_bytes / page_size;
                n_pages = fmin(n_pages, MAX_N_FIND);
                pthread_mutex_lock(&placement_lock);
                n_migrated = send_find(n_pages, NVRAM_MODE);
                pthread_mutex_unlock(&placement_lock);
                if (n_migrated > 0) {
                    sleep_interval *= 3; // give time for bw to settle given the migrated pages
                    printf("NVRAM->DRAM: Migrated %d out of %d pages.\n", n_migrated, n_pages);
                }
            }

            if((nvram_usage > NVRAM_LIMIT) || (dram_usage > DRAM_LIMIT)) {
                any_full = 1;
            }
        }

        if (balance_act && !any_full) {
            time_t memdata_lmod = get_memdata_mtime();
            if (memdata_lmod == 0 || (memdata_lmod == prev_memdata_lmod)) {
                printf("MEMCHECK: Old or invalid memdata values. Ignoring...\n");
            }
            else {
                prev_memdata_lmod = memdata_lmod;
                memdata_t *md = read_memdata();
                if (!check_memdata(md)) {
                    printf("MEMCHECK: Unexpected memdata values.\n");
                }
                else {
                    float dram_bw = md->sys_dramWrites + md->sys_dramReads;
                    float nvram_bw = md->sys_pmmWrites + md->sys_pmmReads;
                    //float nvram_app_bw = md->sys_pmmAppBW;
                    float tot_bw = dram_bw + nvram_bw;


                    float dram_opt_bw = tot_bw * BW_RATIO / (1 + BW_RATIO);
                    float nvram_opt_bw = tot_bw / (1 + BW_RATIO);

                    int n_balanced = 0;

                    if (tot_bw > TOT_BW_THRESH) {
                        //TODO: adicionar margem onde nao se faz nada
                        if ((dram_bw > dram_opt_bw) && (nvram_bw < NVRAM_BW_LIMIT)) {
                            float to_free_bw = dram_bw - dram_opt_bw;
                            int to_free_permill = (to_free_bw / dram_bw) * 1000;
                            pthread_mutex_lock(&placement_lock);
                            n_balanced = send_find(to_free_permill, BALANCE_DRAM_MODE);
                            pthread_mutex_unlock(&placement_lock);
                            if (n_balanced > 0) {
                                sleep_interval *= 3; // give time for bw to settle given the migrated pages
                                printf("DRAM->NVRAM: Balanced %d pages out of %.2f%%.\n", n_balanced, (float) to_free_permill/10);
                            }
                        }
                        else {
                            int nvram_app_bw;
                            if (PMM_MIXED) {
                                nvram_app_bw = md->sys_pmmAppBW;
                            }
                            else {
                                nvram_app_bw = nvram_bw;
                            }

                            if ((dram_bw < DRAM_BW_LIMIT) && (nvram_app_bw > 0.0)) {
                                float to_free_bw = nvram_bw - nvram_opt_bw;
                                int to_free_permill = fmin((to_free_bw / nvram_app_bw) * 1000, 1000);
                                pthread_mutex_lock(&placement_lock);
                                n_balanced = send_find(to_free_permill, BALANCE_NVRAM_MODE);
                                pthread_mutex_unlock(&placement_lock);
                                if (n_balanced > 0) {
                                    printf("NVRAM->DRAM: Balanced %d pages out of %.2f%%.\n", n_balanced, (float) to_free_permill/10);
                                }
                            }
                        }
                    }
                }
                free(md);
            }
        }

        usleep(sleep_interval);
    }

    return NULL;
}

void *nvramWrChk_placement(void *args) {
    time_t prev_memdata_lmod = 0;
    while (!exit_sig) {
        int sleep_interval = nvramWrChk_interval;
        if (nvramWrChk_act) {
            time_t memdata_lmod = get_memdata_mtime();
            if (memdata_lmod == 0 || (memdata_lmod == prev_memdata_lmod)) {
                printf("NVRAMWRCHK: Old or invalid memdata values. Ignoring...\n");
            }
            else {
                prev_memdata_lmod = memdata_lmod;
                memdata_t *md = read_memdata();
                if (!check_memdata(md)) {
                    printf("NVRAMWRCHK: Unexpected memdata values.\n");
                }
                else {
                    float pmm_bw;
                    if (PMM_MIXED) {
                        // If mixed configuration (AD+MM), pcm cannot isolate PMM AD write BW, so use total AD BW
                        pmm_bw = md->sys_pmmAppBW;
                    }
                    else {
                        pmm_bw = md->sys_pmmWrites;
                    }

                    if ((pmm_bw > NVRAM_BW_WR_THRESH)) {

                        int n_avail_pages = free_space_tot_pages(DRAM_MODE);
                        int n_find = fmin(n_avail_pages, MAX_N_FIND);

                        pthread_mutex_lock(&placement_lock);
                        send_find(0, NVRAM_CLEAR_DIRTY);
                        usleep(clearDirty_interval);
                        int n_migrated = send_find(n_find, NVRAM_WRITE_MODE);
                        pthread_mutex_unlock(&placement_lock);

                        if (n_migrated > 0) {
                            sleep_interval *= 3; // give time for bw to settle given the migrated pages
                            printf("NVRAM->DRAM: Sent %d out of %d write-only pages.\n", n_migrated, n_find);
                        }
                        sleep_interval -= clearDirty_interval;
                    }
                }
                free(md);
            }
        }

        usleep(sleep_interval);
    }

    return NULL;
}



/*
-------------------------------------------------------------------------------

STDIN/SOCKET PROCESSING

-------------------------------------------------------------------------------
*/


void *process_stdin(void *args) {
    char *command = malloc(sizeof(char) * MAX_COMMAND_SIZE);
    char *substring;
    long pid;

    printf("Available commands:\n"
            "\tbind [pid]\n"
            "\tunbind [pid]\n"
            "\tDEBUG: send [n] [dram|nvram]\n"
            "\tDEBUG: nvramwrchk [n]\n"
            "\tDEBUG: bal [n]\n"
            "\tDEBUG: toggle [nvramwrchk|thresh|bal|all]\n"
            "\tDEBUG: clear\n"
            "\texit\n");

    while ((fgets(command, MAX_COMMAND_SIZE, stdin) != NULL) && strcmp(command, "exit\n")) {
        if ((substring = strtok(command, " ")) == NULL) {
            continue;
        }

        if (!strcmp(substring, "bind")) {
            if ((substring = strtok(NULL, " ")) == NULL) {
                fprintf(stderr, "Invalid argument for bind command.\n");
                continue;
            }
            pid = strtol(substring, NULL, 10);
            if ((pid>0) && (pid<MAX_PID_N)) {
                if (send_bind((int) pid)) {
                    printf("Bind request success (pid=%d).\n", (int) pid);
                }
                else {
                    fprintf(stderr, "Bind request failed (pid=%d).\n", (int) pid);
                }
            }
            else {
                fprintf(stderr, "Invalid argument for bind command.\n");
            }
        }

        else if (!strcmp(substring, "unbind")) {
            if ((substring = strtok(NULL, " ")) == NULL) {
                fprintf(stderr, "Invalid argument for unbind command.\n");
                continue;
            }
            pid = strtol(substring, NULL, 10);
            if ((pid>0) && (pid<MAX_PID_N)) {
                if (send_unbind((int) pid)) {
                    printf("Unbind request success (pid=%d).\n", (int) pid);
                }
                else {
                    fprintf(stderr, "Unbind request failed (pid=%d).\n", (int) pid);
                }
            }
            else {
                fprintf(stderr, "Invalid argument for unbind command.\n");
            }
        }

        else if (!strcmp(substring, "send")) {
            if ((substring = strtok(NULL, " ")) == NULL) {
                fprintf(stderr, "Invalid argument for send command.\n");
                continue;
            }
            long n = strtol(substring, NULL, 10);
            if ((substring = strtok(NULL, " ")) == NULL) {
                fprintf(stderr, "Invalid argument for send command.\n");
                continue;
            }

            int n_migrated = 0;

            if (!strcmp(substring, "dram\n")) {
                pthread_mutex_lock(&placement_lock);
                n_migrated = send_find((int) n, NVRAM_MODE);
                pthread_mutex_unlock(&placement_lock);
            }
            else if (!strcmp(substring, "nvram\n")) {
                pthread_mutex_lock(&placement_lock);
                n_migrated = send_find((int) n, DRAM_MODE);
                pthread_mutex_unlock(&placement_lock);
            }

            else {
                fprintf(stderr, "Invalid argument for send command.\n");
                continue;
            }
            if (n_migrated > 0) {
                printf("Migrated %d out of %ld pages.\n", n_migrated, n);
            }
        }

        else if (!strcmp(substring, "nvramwrchk")) {
            if ((substring = strtok(NULL, " ")) == NULL) {
                fprintf(stderr, "Invalid argument for nvramwrchk command.\n");
                continue;
            }
            long n = strtol(substring, NULL, 10);
            pthread_mutex_lock(&placement_lock);
            int n_migrated = send_find((int) n, NVRAM_WRITE_MODE);
            pthread_mutex_unlock(&placement_lock);
            if (n_migrated > 0) {
                printf("NVRAM->DRAM: Sent %d out of %ld write-only pages.\n", n_migrated, n);
            }
        }

        else if (!strcmp(substring, "baldram")) {
            if ((substring = strtok(NULL, " ")) == NULL) {
                fprintf(stderr, "Invalid argument for balance command.\n");
                continue;
            }
            long n = strtol(substring, NULL, 10);
            pthread_mutex_lock(&placement_lock);
            int n_balanced = send_find((int) n, BALANCE_DRAM_MODE);
            pthread_mutex_unlock(&placement_lock);
            if (n_balanced > 0) {
                printf("DRAM->NVRAM: Balanced %d pages out of %.2f%%.\n", n_balanced, (float) n/10);
            }
        }

        else if (!strcmp(substring, "balnvram")) {
            if ((substring = strtok(NULL, " ")) == NULL) {
                fprintf(stderr, "Invalid argument for balance command.\n");
                continue;
            }
            long n = strtol(substring, NULL, 10);
            pthread_mutex_lock(&placement_lock);
            int n_balanced = send_find((int) n, BALANCE_NVRAM_MODE);
            pthread_mutex_unlock(&placement_lock);
            if (n_balanced > 0) {
                printf("NVRAM->DRAM: Balanced %d pages out of %.2f%%.\n", n_balanced, (float) n/10);
            }
        }

        else if (!strcmp(substring, "toggle")) {
            if ((substring = strtok(NULL, " ")) == NULL) {
                fprintf(stderr, "Invalid argument for toggle command.\n");
                continue;
            }
            if (!strcmp(substring, "nvramwrchk\n")) {
                nvramWrChk_act = 1 - nvramWrChk_act;

                if (nvramWrChk_act) {
                    printf("Nvram WrChk component turned ON\n");
                }
                else {
                    printf("Nvram WrChk component turned OFF\n");
                }
            }
            else if (!strcmp(substring, "thresh\n")) {
                thresh_act = 1 - thresh_act;

                if (thresh_act) {
                    printf("Threshold component turned ON\n");
                }
                else {
                    printf("Threshold component turned OFF\n");
                }
            }
            else if (!strcmp(substring, "bal\n")) {
                balance_act = 1 - balance_act;

                if (balance_act) {
                    printf("Balance component turned ON\n");
                }
                else {
                    printf("Balance component turned OFF\n");
                }
            }
            else if (!strcmp(substring, "all\n")) {
                nvramWrChk_act = 1 - nvramWrChk_act;
                thresh_act = 1 - thresh_act;
                balance_act = 1 - balance_act;

                if (nvramWrChk_act) {
                    printf("Nvram WrChk component turned ON\n");
                }
                else {
                    printf("Nvram WrChk component turned OFF\n");
                }

                if (thresh_act) {
                    printf("Threshold component turned ON\n");
                }
                else {
                    printf("Threshold component turned OFF\n");
                }

                if (balance_act) {
                    printf("Balance component turned ON\n");
                }
                else {
                    printf("Balance component turned OFF\n");
                }
            }
        }

        else if (!strcmp(substring, "clr\n") || !strcmp(substring, "clear\n")) {
            system("@cls||clear");
        }

        else {
            fprintf(stderr, "Unknown command.\n"
                    "Available commands:\n"
                    "\tbind [pid]\n"
                    "\tunbind [pid]\n"
                    "\tDEBUG: send [n] [dram|nvram]\n"
                    "\tDEBUG: nvramwrchk [n]\n"
                    "\tDEBUG: [baldram|balnvram] [n](permillage)\n"
                    "\tDEBUG: toggle [nvramwrchk|thresh|bal|all]\n"
                    "\tDEBUG: clear\n"
                    "\texit\n");

        }

        //TODO: add console debug commands here

        // Testar modificar paginas "quentes"
        // Criar testes sinteticos.
        //
    }
    exit_sig = 1;
    free(command);

    return NULL;
}

void *process_socket(void *args) {
    // Unix domain socket
    struct sockaddr_un uds_addr;
    int unix_fd, sel, acc, rd;
    req_t unix_req;

    struct timeval sel_timeout;
    fd_set readfds;


    if ((unix_fd = socket(AF_UNIX, SOCK_STREAM, 0)) == -1) {
        fprintf(stderr, "Error creating UD socket: %s\n", strerror(errno));
        return NULL;
    }
    memset(&uds_addr, 0, sizeof(uds_addr));
    uds_addr.sun_family = AF_UNIX;

    strncpy(uds_addr.sun_path, UDS_path, sizeof(uds_addr.sun_path)-1);
    unlink(UDS_path); // unlink to avoid error in bind

    if (bind(unix_fd, (struct sockaddr*)&uds_addr, sizeof(uds_addr)) == -1) {
        fprintf(stderr, "Error binding UDS: %s\n", strerror(errno));
        return NULL;
    }

    if (listen(unix_fd, MAX_BACKLOG) == -1) {
        fprintf(stderr, "Error marking UDS as passive: %s\n", strerror(errno));
        return NULL;
    }

    while (!exit_sig) {
        sel_timeout.tv_sec = SELECT_TIMEOUT;

        FD_ZERO(&readfds);
        FD_SET(unix_fd, &readfds);
        sel = select(unix_fd+1, &readfds, NULL, NULL, &sel_timeout);

        if (sel == -1) {
            fprintf(stderr, "Error in UDS select: %s.\n", strerror(errno));
            return NULL;
        } else if ((sel > 0) && FD_ISSET(unix_fd, &readfds)) {
            if ((acc = accept(unix_fd, NULL, NULL)) == -1) {
                fprintf(stderr, "Failed accepting incoming UDS connection: %s\n", strerror(errno));
                continue;
            }
            while ( (rd = read(acc, &unix_req,sizeof(req_t))) == sizeof(req_t)) {
                switch (unix_req.op_code) {
                    case BIND_OP:
                        if (send_bind(unix_req.pid_n)) {
                            printf("Bind request success (pid=%d).\n", unix_req.pid_n);
                        }
                        else {
                            fprintf(stderr, "Bind request failed (pid=%d).\n", unix_req.pid_n);
                        }
                        break;
                    case UNBIND_OP:
                        if (send_unbind(unix_req.pid_n)) {
                            printf("Unbind request success (pid=%d).\n", unix_req.pid_n);
                        }
                        else {
                            fprintf(stderr, "Unbind request failed (pid=%d).\n", unix_req.pid_n);
                        }
                        break;
                    default:
                        fprintf(stderr, "Unexpected request OPcode from accepted UD socket connection");
                }
            }

            if (rd < 0) {
                fprintf(stderr, "Error reading from accepted UDS connection: %s\n", strerror(errno));
                return NULL;
            }

            else if (rd == 0) {
                close(acc);
            }
            else {
                fprintf(stderr, "Unexpected amount of bytes read from accepted UD socket connection.\n");
            }
        }
    }
    unlink(UDS_path);
    return NULL;
}



/*
-------------------------------------------------------------------------------

MAIN FUNCTION

-------------------------------------------------------------------------------
*/


int main() {

    if ((netlink_fd = socket(PF_NETLINK, SOCK_RAW, NETLINK_USER)) == -1) {
        fprintf(stderr, "Could not create netlink socket fd: %s\nTry inserting kernel module first.\n", strerror(errno));
        return 1;
    }
    page_size = sysconf(_SC_PAGESIZE);
    int packet_size = NLMSG_SPACE(MAX_PAYLOAD);
    buf_size = packet_size * MAX_PACKETS;

    candidates = malloc(sizeof(addr_info_t) * MAX_N_FIND);

    buffer = malloc(buf_size);

    nlmh_out = malloc(packet_size);

    configure_netlink_addr();
    configure_netlink_outbound();
    configure_netlink_inbound();

    if (bind(netlink_fd, (struct sockaddr *) &src_addr, sizeof(src_addr))) {
        printf("Error binding netlink socket fd: %s\n", strerror(errno));
        free(candidates);
        free(buffer);
        free(nlmh_out);
        return 1;
    }

    if (pthread_mutex_init(&comm_lock, NULL)) {
        fprintf(stderr, "Error creating communication mutex lock: %s\n", strerror(errno));
    }

    else if (pthread_mutex_init(&placement_lock, NULL)) {
        fprintf(stderr, "Error creating placement mutex lock: %s\n", strerror(errno));
    }

    else if (pthread_create(&stdin_thread, NULL, process_stdin, NULL)) {
        fprintf(stderr, "Error spawning stdin thread: %s\n", strerror(errno));
    }

    else if (pthread_create(&socket_thread, NULL, process_socket, NULL)) {
        fprintf(stderr, "Error spawning socket thread: %s\n", strerror(errno));
    }

    else if (pthread_create(&memcheck_thread, NULL, memcheck_placement, NULL)) {
        fprintf(stderr, "Error spawning memcheck placement thread: %s\n", strerror(errno));
    }

    else if (pthread_create(&nvramWrChk_thread, NULL, nvramWrChk_placement, NULL)) {
        fprintf(stderr, "Error spawning nvram write check thread: %s\n", strerror(errno));
    }

    else {
        pthread_join(stdin_thread, NULL);
        printf("Exiting ctl...\n");
        pthread_join(socket_thread, NULL);
        pthread_join(memcheck_thread, NULL);
        pthread_join(nvramWrChk_thread, NULL);

        pthread_mutex_destroy(&comm_lock);
        pthread_mutex_destroy(&placement_lock);

        close(netlink_fd);
        free(candidates);
        free(buffer);
        free(nlmh_out);
        return 0;
    }
    close(netlink_fd);
    free(candidates);
    free(buffer);
    free(nlmh_out);
    return 1;
}
