/*****************************************************************************
Copyright 2012 Laboratory for Advanced Computing at the University of Chicago

This file is part of UDR.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing,
software distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions
and limitations under the License.
*****************************************************************************/

#include <unistd.h>
#include <cstdlib>
#include <cstring>
#include <stdio.h>
#include <stdlib.h>
#include <getopt.h>
#include "udr_options.h"

#include <sys/stat.h>
#include <sys/types.h>

using namespace std;

void usage() {
    fprintf(stderr, "usage: udr [UDR options] rsync [rsync options]\n\n");
    fprintf(stderr, "UDR options:\n");
    fprintf(stderr, "\t[-n aes-128 | aes-192 | aes-256 | bf | des-ede3] Encryption cypher\n");
    fprintf(stderr, "\t[-v] Run UDR with verbosity\n");
    fprintf(stderr, "\t[-d timeout] Idle timeout seconds with no pipe data (default 600; 0=disable)\n");
    fprintf(stderr, "\t[-a port] Local UDT port\n");
    fprintf(stderr, "\t[-b port] Remote UDT port\n");
    fprintf(stderr, "\t[-c path] Remote UDR executable\n");
    fprintf(stderr, "\t[-P ssh-port] Remote port to connect to via SSH\n");
    fprintf(stderr, "\t[--udt-buf BYTES] UDT send/recv buffer (default 128M; accepts K/M/G suffix)\n");
    fprintf(stderr, "\t[--udp-buf BYTES] Kernel UDP send/recv buffer (default 16M)\n");
    fprintf(stderr, "\t[--udt-flight N]  UDT flight window in packets (default: auto)\n");
    exit(1);
}

// Parse sizes like "128M", "65536", "2G". Returns bytes, or -1 on error.
static int parse_size_arg(const char *s) {
    if (!s || !s[0])
        return -1;
    char *end = NULL;
    double v = strtod(s, &end);
    if (end == s || v < 0)
        return -1;
    if (*end == '\0')
        return (int)v;
    if ((end[0] == 'K' || end[0] == 'k') && end[1] == '\0')
        return (int)(v * 1024.0);
    if ((end[0] == 'M' || end[0] == 'm') && end[1] == '\0')
        return (int)(v * 1024.0 * 1024.0);
    if ((end[0] == 'G' || end[0] == 'g') && end[1] == '\0')
        return (int)(v * 1024.0 * 1024.0 * 1024.0);
    return -1;
}

void set_default_udr_options(UDR_Options * options) {
    options->ssh_port = 22;
    options->start_port = 9000;
    options->end_port = 9100;
    // Idle gap with no UDT/rsync pipe traffic before the receiver aborts.
    // 15s is too short for --partial/--inplace resume on large files (rsync
    // can think for minutes while checking existing data).
    options->timeout = 600;

    options->tflag = false;
    options->sflag = false;
    options->verbose = false;
    options->encryption = false;
    options->version_flag = false;
    options->server_connect = false;

    snprintf(options->udr_program_dest, PATH_MAX, "%s", "udr");
    snprintf(options->ssh_program, PATH_MAX, "%s", "ssh");
    snprintf(options->rsync_program, PATH_MAX, "%s", "rsync");
    snprintf(options->rsync_timeout, PATH_MAX, "%s", "--timeout=0");
    snprintf(options->shell_program, PATH_MAX, "%s", "sh");
    snprintf(options->key_base_filename, PATH_MAX, "%s", ".udr_key");
    options->key_filename[0] = '\0';

    options->host[0] = '\0';
    options->username[0] = '\0';
    options->which_process[0] = '\0';
    options->version[0] = '\0';
    options->server_dir[0] = '\0';
    options->server_config[0] = '\0';
    snprintf(options->server_port, NI_MAXSERV, "%s", "9000");

    options->specify_ip = NULL;

    // Defaults sized for multi-hundred-Mbps WAN / high RTT (BDP), not LAN.
    // Stock UDT uses ~12MB UDT buffers and 64KB UDP send — far too small overseas.
    options->udt_buf_size = 128 * 1024 * 1024;  // 128 MiB
    options->udp_buf_size = 16 * 1024 * 1024;   // 16 MiB
    options->udt_flight = 0;                    // auto from udt_buf

    options->rsync_uid = 0;
    options->rsync_gid = 0;
}

int get_udr_options(UDR_Options * udr_options, int argc, char * argv[], int rsync_arg_idx) {
    int ch;
    char *key_dir = NULL;

    set_default_udr_options(udr_options);

    snprintf(udr_options->udr_program_src, PATH_MAX, "%s", argv[0]);

    static struct option long_options[] = {
        {"verbose", no_argument, NULL, 'v'},
        {"version", no_argument, NULL, 0},
        {"ssh-port", optional_argument, NULL, 'P'},
        {"start-port", required_argument, NULL, 'a'},
        {"end-port", required_argument, NULL, 'b'},
        {"receiver", no_argument, NULL, 't'},
        {"server", required_argument, NULL, 'd'},
        {"encrypt", optional_argument, NULL, 'n'},
        {"sender", no_argument, NULL, 's'},
        {"login-name", required_argument, NULL, 'l'},
        {"keyfile", required_argument, NULL, 'p'},
        {"keydir", required_argument, NULL, 'k'},
        {"remote-udr", required_argument, NULL, 'c'},
        {"server-port", required_argument, NULL, 'o'},
        {"rsync-uid", required_argument, NULL, 0},
        {"rsync-gid", required_argument, NULL, 0},
        {"config", required_argument, NULL, 0},
        {"udt-buf", required_argument, NULL, 0},
        {"udp-buf", required_argument, NULL, 0},
        {"udt-flight", required_argument, NULL, 0},
        {0, 0, 0, 0}
    };

    int option_index = 0;

    const char* opts = "P:i:tlvxa:b:s:d:h:p:c:k:o:n::";

    while ((ch = getopt_long(rsync_arg_idx, argv, opts, long_options, &option_index)) != -1) {
        switch (ch) {
        case 'P':
            udr_options->ssh_port = atoi(optarg);
            break;
        case 'a':
            udr_options->start_port = atoi(optarg);
            break;
        case 'd':
            udr_options->timeout = atoi(optarg);
            break;
        case 'b':
            udr_options->end_port = atoi(optarg);
            break;
        case 't':
            udr_options->tflag = 1;
            break;
        case 'n':
            udr_options->encryption = true;
            if (optarg) {
            	snprintf(udr_options->encryption_type, PATH_MAX, "%s", optarg);
            } else {
                snprintf(udr_options->encryption_type, PATH_MAX, "%s", "aes-128");
            }
            break;
        case 's':
            udr_options->sflag = 1;
            snprintf(udr_options->port_num, NI_MAXSERV, "%s", optarg);
            break;
        case 'l':
            snprintf(udr_options->username, PATH_MAX, "%s", optarg);
            break;
        case 'p':
            snprintf(udr_options->key_filename, PATH_MAX, "%s", optarg);
            break;
        case 'c':
            snprintf(udr_options->udr_program_dest, PATH_MAX, "%s", optarg);
            break;
        case 'k':
            key_dir = optarg;
            break;
        case 'v':
            udr_options->verbose = true;
            break;
        case 'o':
            snprintf(udr_options->server_port, NI_MAXSERV, "%s", optarg);
            break;

        case 'i':
            udr_options->specify_ip = strdup(optarg);
            break;

        case 'x':
            udr_options->server_connect = true;
        case 0:
            if (strcmp("version", long_options[option_index].name) == 0) {
                udr_options->version_flag = true;
            }
            else if (strcmp("config", long_options[option_index].name) == 0){
                snprintf(udr_options->server_config, PATH_MAX, "%s", optarg);
            }
            else if (strcmp("rsync-uid", long_options[option_index].name) == 0){
                udr_options->rsync_uid = atoi(optarg);
            }
            else if (strcmp("rsync-gid", long_options[option_index].name) == 0){
                udr_options->rsync_gid = atoi(optarg);
            }
            else if (strcmp("udt-buf", long_options[option_index].name) == 0) {
                int n = parse_size_arg(optarg);
                if (n < 65536) {
                    fprintf(stderr, "UDR ERROR: --udt-buf must be >= 64K\n");
                    exit(1);
                }
                udr_options->udt_buf_size = n;
            }
            else if (strcmp("udp-buf", long_options[option_index].name) == 0) {
                int n = parse_size_arg(optarg);
                if (n < 65536) {
                    fprintf(stderr, "UDR ERROR: --udp-buf must be >= 64K\n");
                    exit(1);
                }
                udr_options->udp_buf_size = n;
            }
            else if (strcmp("udt-flight", long_options[option_index].name) == 0) {
                udr_options->udt_flight = atoi(optarg);
                if (udr_options->udt_flight < 32) {
                    fprintf(stderr, "UDR ERROR: --udt-flight must be >= 32\n");
                    exit(1);
                }
            }
            break;
        default:
            fprintf(stderr, "Illegal argument: %c\n", ch);
            usage();
        }
    }

    // verify that timeout duration > 0
    if (udr_options->timeout < 1){
	fprintf(stderr, "Please specify a timeout duration [-d timeout] greater than 0s.\n");
	exit(1);
    }

    //Finish setting up the key file path
    if (key_dir == NULL) {
        snprintf(udr_options->key_filename, PATH_MAX, "%s", udr_options->key_base_filename);
    } else {
        sprintf(udr_options->key_filename, "%s/%s", key_dir, udr_options->key_base_filename);
    }

    //Set which_process for debugging output
    if (udr_options->verbose) {
        if (udr_options->sflag)
            snprintf(udr_options->which_process, PATH_MAX, "%s", "[udr sender]");
        else if (udr_options->tflag)
            snprintf(udr_options->which_process, PATH_MAX, "%s", "[udr receiver]");
        else
            snprintf(udr_options->which_process, PATH_MAX, "%s", "[udr original]");

        fprintf(stderr, "%s Local program: %s Remote program: %s Encryption: %d\n", udr_options->which_process, udr_options->udr_program_src, udr_options->udr_program_dest, udr_options->encryption);
    }

    //check that -e/--rsh flag has not been used with rsync
    for(int i = rsync_arg_idx; i < argc; i++){
        if(strcmp(argv[i], "-e") == 0 || strcmp(argv[i], "--rsh") == 0){
            fprintf(stderr, "UDR ERROR: UDR overrides the -e, --rsh flag of rsync, so they cannot be used in the provided rsync command\n");
            exit(1);
        }
    }

    return 1;
}

// Normalize host for SSH/getaddrinfo: strip surrounding [ ] used in rsync IPv6 syntax.
static void copy_host_normalized(char* dest, const char* src, size_t len) {
    if (len > PATH_MAX)
        len = PATH_MAX;

    if (len >= 2 && src[0] == '[' && src[len - 1] == ']') {
        // Bracketed IPv6 literal: store without brackets
        size_t inner = len - 2;
        if (inner > PATH_MAX)
            inner = PATH_MAX;
        memcpy(dest, src + 1, inner);
        dest[inner] = '\0';
    } else {
        memcpy(dest, src, len);
        dest[len] = '\0';
    }
}

void parse_host_username(char* source, char* username, char* host, bool* double_colon){
    // rsync IPv6 forms:
    //   user@[2001:db8::1]:/path
    //   [2001:db8::1]:/path
    //   user@[2001:db8::1]::module
    // classic forms:
    //   user@host:/path
    //   host:/path
    //   user@host::module
    char * at_loc = strchr(source, '@');
    char * bracket_open = strchr(source, '[');
    char * bracket_close = bracket_open ? strchr(bracket_open, ']') : NULL;

    username[0] = '\0';
    host[0] = '\0';
    *double_colon = false;

    if (bracket_open && bracket_close && bracket_close > bracket_open) {
        // Host is the IPv6 literal inside [ ]
        // Optional username must appear before '[' (user@[...])
        if (at_loc != NULL && at_loc < bracket_open) {
            size_t username_len = (size_t)(at_loc - source);
            if (username_len > PATH_MAX) {
                fprintf(stderr, "UDR ERROR: username_len > PATH_MAX\n");
                exit(1);
            }
            memcpy(username, source, username_len);
            username[username_len] = '\0';
        } else if (at_loc != NULL && at_loc > bracket_close) {
            // Invalid placement of @
            return;
        }

        size_t host_len = (size_t)(bracket_close - bracket_open - 1);
        if (host_len == 0 || host_len > PATH_MAX) {
            fprintf(stderr, "UDR ERROR: invalid IPv6 host length\n");
            exit(1);
        }
        memcpy(host, bracket_open + 1, host_len);
        host[host_len] = '\0';

        // Path / module separator is after ']'
        if (bracket_close[1] == ':') {
            if (bracket_close[2] == ':')
                *double_colon = true;
        } else {
            // No ':' after brackets → not a remote rsync path form we handle
            host[0] = '\0';
            username[0] = '\0';
        }
        return;
    }

    // Non-bracketed: first ':' separates host from path (hostnames / IPv4 only)
    char * colon_loc = strchr(source, ':');
    if (colon_loc == NULL) {
        return;
    }

    if (colon_loc[1] == ':') {
        *double_colon = true;
    }

    if (at_loc != NULL && at_loc < colon_loc) {
        size_t host_len = (size_t)(colon_loc - at_loc - 1);
        size_t username_len = (size_t)(at_loc - source);

        if (host_len > PATH_MAX) {
            fprintf(stderr, "UDR ERROR: host_len > PATH_MAX\n");
            exit(1);
        }
        if (username_len > PATH_MAX) {
            fprintf(stderr, "UDR ERROR: username_len > PATH_MAX\n");
            exit(1);
        }

        copy_host_normalized(host, at_loc + 1, host_len);
        memcpy(username, source, username_len);
        username[username_len] = '\0';
    } else {
        size_t host_len = (size_t)(colon_loc - source);
        if (host_len > PATH_MAX)
            host_len = PATH_MAX;
        copy_host_normalized(host, source, host_len);
    }
}

//Gets the host and username by parsing the rsync options
void get_host_username(UDR_Options * udr_options, int argc, char *argv[], int rsync_arg_idx){
    bool src_remote = true;
    bool dest_remote = true;

    //destination is always the last one
    char dest_username[PATH_MAX+1];
    char dest_host[PATH_MAX+1];
    bool dest_double_colon = false;
    dest_username[0] = '\0';
    dest_host[0] = '\0';

    char next_src_username[PATH_MAX+1];
    char next_src_host[PATH_MAX+1];
    bool next_src_double_colon = false;
    next_src_username[0] = '\0';
    next_src_host[0] = '\0';

    char src_username[PATH_MAX+1];
    char src_host[PATH_MAX+1];
    bool src_double_colon = false;
    src_username[0] = '\0';
    src_host[0] = '\0';

    int src_username_len, src_host_len, dest_username_len, dest_host_len;

    char * dest = argv[argc-1];

    //go backwards until find first option, we'll call those the source
    int src_num = 0;
    for(int i = argc-2; i > rsync_arg_idx; i--){
//        fprintf(stderr, "i: %d argv: %s\n", i, argv[i]);
        if(argv[i][0] == '-'){
            break;
        }
        else{
//            fprintf(stderr, "parsing: %s\n", argv[i]);
//            fprintf(stderr, "src username: %s\n", src_username );
//            fprintf(stderr, "src host: %s\n", src_host);
            parse_host_username(argv[i], next_src_username, next_src_host, &next_src_double_colon);
//            fprintf(stderr, "next src username: %s\n", next_src_username );
//            fprintf(stderr, "next src host: %s\n", next_src_host);
            if(src_num != 0){
                if(strcmp(src_username,next_src_username) != 0 || strcmp(src_host,next_src_host) != 0 || src_double_colon != next_src_double_colon){
                    //have a problem
                    fprintf(stderr, "UDR ERROR: source must use the same host and username\n");
                    exit(-1);
                }
            }
            snprintf(src_username, PATH_MAX, "%s", next_src_username);
            snprintf(src_host, PATH_MAX, "%s", next_src_host);
            src_double_colon = next_src_double_colon;
            next_src_username[0] = '\0';
            next_src_host[0] = '\0';
            next_src_double_colon = false;
            src_num++;
        }
    }


//    fprintf(stderr, "src_username: %s src_host: %s\n", src_username, src_host);

    if(strlen(src_host) == 0){
        src_remote = false;
    }

//    fprintf(stderr, "dest: %s\n", dest);
    parse_host_username(dest, dest_username, dest_host, &dest_double_colon);

//    fprintf(stderr, "dest_username: %s dest_host: %s\n", dest_username, dest_host);

    if(strlen(dest_host) == 0){
        dest_remote = false;
    }

//    fprintf(stderr, "src_remote: %d dest_remote: %d\n", src_remote, dest_remote);

    if(src_remote == dest_remote){
        fprintf(stderr, "UDR ERROR: UDR only does remote -> local or local -> remote transfers\n");
        exit(-1);
    }

    if(src_remote){
        snprintf(udr_options->host, PATH_MAX, "%s", src_host);
        snprintf(udr_options->username, PATH_MAX, "%s", src_username);
        udr_options->server_connect = src_double_colon;
    }
    else{
        snprintf(udr_options->host, PATH_MAX, "%s", dest_host);
        snprintf(udr_options->username, PATH_MAX, "%s", dest_username);
        udr_options->server_connect = dest_double_colon;
    }
}
