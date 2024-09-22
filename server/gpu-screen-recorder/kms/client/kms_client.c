#include "kms_client.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <stdbool.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/capability.h>
#include <sys/random.h>

#define GSR_SOCKET_PAIR_LOCAL  0
#define GSR_SOCKET_PAIR_REMOTE 1

static void cleanup_socket(gsr_kms_client *self, bool kill_server);
static int gsr_kms_client_replace_connection(gsr_kms_client *self);

static bool generate_random_characters(char *buffer, int buffer_size, const char *alphabet, size_t alphabet_size) {
    /* TODO: Use other functions on other platforms than linux */
    if(getrandom(buffer, buffer_size, 0) < buffer_size) {
        fprintf(stderr, "Failed to get random bytes, error: %s\n", strerror(errno));
        return false;
    }

    for(int i = 0; i < buffer_size; ++i) {
        unsigned char c = *(unsigned char*)&buffer[i];
        buffer[i] = alphabet[c % alphabet_size];
    }

    return true;
}

static void close_fds(gsr_kms_response *response) {
    for(int i = 0; i < response->num_items; ++i) {
        for(int j = 0; j < response->items[i].num_dma_bufs; ++j) {
            gsr_kms_response_dma_buf *dma_buf = &response->items[i].dma_buf[j];
            if(dma_buf->fd > 0) {
                close(dma_buf->fd);
                dma_buf->fd = -1;
            }
        }
        response->items[i].num_dma_bufs = 0;
    }
    response->num_items = 0;
}

static int send_msg_to_server(int server_fd, gsr_kms_request *request) {
    struct iovec iov;
    iov.iov_base = request;
    iov.iov_len = sizeof(*request);

    struct msghdr response_message = {0};
    response_message.msg_iov = &iov;
    response_message.msg_iovlen = 1;

    char cmsgbuf[CMSG_SPACE(sizeof(int) * 1)];
    memset(cmsgbuf, 0, sizeof(cmsgbuf));

    if(request->new_connection_fd > 0) {
        response_message.msg_control = cmsgbuf;
        response_message.msg_controllen = sizeof(cmsgbuf);

        struct cmsghdr *cmsg = CMSG_FIRSTHDR(&response_message);
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;
        cmsg->cmsg_len = CMSG_LEN(sizeof(int) * 1);

        int *fds = (int*)CMSG_DATA(cmsg);
        fds[0] = request->new_connection_fd;

        response_message.msg_controllen = cmsg->cmsg_len;
    }

    return sendmsg(server_fd, &response_message, 0);
}

static int recv_msg_from_server(int server_pid, int server_fd, gsr_kms_response *response) {
    struct iovec iov;
    iov.iov_base = response;
    iov.iov_len = sizeof(*response);

    struct msghdr response_message = {0};
    response_message.msg_iov = &iov;
    response_message.msg_iovlen = 1;

    char cmsgbuf[CMSG_SPACE(sizeof(int) * GSR_KMS_MAX_ITEMS * GSR_KMS_MAX_DMA_BUFS)];
    memset(cmsgbuf, 0, sizeof(cmsgbuf));
    response_message.msg_control = cmsgbuf;
    response_message.msg_controllen = sizeof(cmsgbuf);

    int res = 0;
    for(;;) {
        res = recvmsg(server_fd, &response_message, MSG_DONTWAIT);
        if(res <= 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            // If we are replacing the connection and closing the application at the same time
            // then recvmsg can get stuck (because the server died), so we prevent that by doing
            // non-blocking recvmsg and checking if the server died
            int status = 0;
            int wait_result = waitpid(server_pid, &status, WNOHANG);
            if(wait_result != 0) {
                res = -1;
                break;
            }
            usleep(1000);
        } else {
            break;
        }
    }

    if(res > 0 && response->num_items > 0) {
        struct cmsghdr *cmsg = CMSG_FIRSTHDR(&response_message);
        if(cmsg) {
            int *fds = (int*)CMSG_DATA(cmsg);
            int fd_index = 0;
            for(int i = 0; i < response->num_items; ++i) {
                for(int j = 0; j < response->items[i].num_dma_bufs; ++j) {
                    gsr_kms_response_dma_buf *dma_buf = &response->items[i].dma_buf[j];
                    dma_buf->fd = fds[fd_index++];
                }
            }
        } else {
            close_fds(response);
        }
    }

    return res;
}

/* We have to use $HOME because in flatpak there is no simple path that is accessible, read and write, that multiple flatpak instances can access */
static bool create_socket_path(char *output_path, size_t output_path_size) {
    const char *home = getenv("HOME");
    if(!home)
        home = "/tmp";

    char random_characters[11];
    random_characters[10] = '\0';
    if(!generate_random_characters(random_characters, 10, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789", 62))
        return false;

    snprintf(output_path, output_path_size, "%s/.gsr-kms-socket-%s", home, random_characters);
    return true;
}

static void string_copy(char *dst, const char *src, int len) {
    int src_len = strlen(src);
    int min_len = src_len;
    if(len - 1 < min_len)
        min_len = len - 1;
    memcpy(dst, src, min_len);
    dst[min_len] = '\0';
}

static bool find_program_in_path(const char *program_name, char *filepath, int filepath_len) {
    const char *path = getenv("PATH");
    if(!path)
        return false;

    int program_name_len = strlen(program_name);
    const char *end = path + strlen(path);
    while(path != end) {
        const char *part_end = strchr(path, ':');
        const char *next = part_end;
        if(part_end) {
            next = part_end + 1;
        } else {
            part_end = end;
            next = end;
        }

        int len = part_end - path;
        if(len + 1 + program_name_len < filepath_len) {
            memcpy(filepath, path, len);
            filepath[len] = '/';
            memcpy(filepath + len + 1, program_name, program_name_len);
            filepath[len + 1 + program_name_len] = '\0';

            if(access(filepath, F_OK) == 0)
                return true;
        }

        path = next;
    }

    return false;
}

int gsr_kms_client_init(gsr_kms_client *self, const char *card_path) {
    int result = -1;
    self->kms_server_pid = -1;
    self->initial_socket_fd = -1;
    self->initial_client_fd = -1;
    self->initial_socket_path[0] = '\0';
    self->socket_pair[0] = -1;
    self->socket_pair[1] = -1;
    struct sockaddr_un local_addr = {0};
    struct sockaddr_un remote_addr = {0};

    if(!create_socket_path(self->initial_socket_path, sizeof(self->initial_socket_path))) {
        fprintf(stderr, "gsr error: gsr_kms_client_init: failed to create path to kms socket\n");
        return -1;
    }

    char server_filepath[PATH_MAX];
    if(!find_program_in_path("gsr-kms-server", server_filepath, sizeof(server_filepath))) {
        fprintf(stderr, "gsr error: gsr_kms_client_init: gsr-kms-server is not installed\n");
        return -1;
    }

    const bool inside_flatpak = getenv("FLATPAK_ID") != NULL;
    const char *home = getenv("HOME");
    if(!home)
        home = "/tmp";

    bool has_perm = 0;
    if(geteuid() == 0) {
        has_perm = true;
    } else {
        cap_t kms_server_cap = cap_get_file(server_filepath);
        if(kms_server_cap) {
            cap_flag_value_t res = CAP_CLEAR;
            cap_get_flag(kms_server_cap, CAP_SYS_ADMIN, CAP_PERMITTED, &res);
            if(res == CAP_SET) {
                //fprintf(stderr, "has permission!\n");
                has_perm = true;
            } else {
                //fprintf(stderr, "No permission:(\n");
            }
            cap_free(kms_server_cap);
        } else if(!inside_flatpak) {
            if(errno == ENODATA)
                fprintf(stderr, "gsr info: gsr_kms_client_init: gsr-kms-server is missing sys_admin cap and will require root authentication. To bypass this automatically, run: sudo setcap cap_sys_admin+ep '%s'\n", server_filepath);
            else
                fprintf(stderr, "gsr info: gsr_kms_client_init: failed to get cap\n");
        }
    }

    if(socketpair(AF_UNIX, SOCK_STREAM, 0, self->socket_pair) == -1) {
        fprintf(stderr, "gsr error: gsr_kms_client_init: socketpair failed, error: %s\n", strerror(errno));
        goto err;
    }

    self->initial_socket_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if(self->initial_socket_fd == -1) {
        fprintf(stderr, "gsr error: gsr_kms_client_init: socket failed, error: %s\n", strerror(errno));
        goto err;
    }

    local_addr.sun_family = AF_UNIX;
    string_copy(local_addr.sun_path, self->initial_socket_path, sizeof(local_addr.sun_path));

    const mode_t prev_mask = umask(0000);
    const int bind_res = bind(self->initial_socket_fd, (struct sockaddr*)&local_addr, sizeof(local_addr.sun_family) + strlen(local_addr.sun_path));
    umask(prev_mask);

    if(bind_res == -1) {
        fprintf(stderr, "gsr error: gsr_kms_client_init: failed to bind socket, error: %s\n", strerror(errno));
        goto err;
    }

    if(listen(self->initial_socket_fd, 1) == -1) {
        fprintf(stderr, "gsr error: gsr_kms_client_init: failed to listen on socket, error: %s\n", strerror(errno));
        goto err;
    }

    pid_t pid = fork();
    if(pid == -1) {
        fprintf(stderr, "gsr error: gsr_kms_client_init: fork failed, error: %s\n", strerror(errno));
        goto err;
    } else if(pid == 0) { /* child */
        if(inside_flatpak) {
            const char *args[] = { "flatpak-spawn", "--host", "/var/lib/flatpak/app/com.dec05eba.gpu_screen_recorder/current/active/files/bin/kms-server-proxy", self->initial_socket_path, card_path, home, NULL };
            execvp(args[0], (char *const*)args);
        } else if(has_perm) {
            const char *args[] = { server_filepath, self->initial_socket_path, card_path, NULL };
            execvp(args[0], (char *const*)args);
        } else {
            const char *args[] = { "pkexec", server_filepath, self->initial_socket_path, card_path, NULL };
            execvp(args[0], (char *const*)args);
        }
        fprintf(stderr, "gsr error: gsr_kms_client_init: execvp failed, error: %s\n", strerror(errno));
        _exit(127);
    } else { /* parent */
        self->kms_server_pid = pid;
    }

    fprintf(stderr, "gsr info: gsr_kms_client_init: waiting for server to connect\n");
    for(;;) {
        struct timeval tv;
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(self->initial_socket_fd, &rfds);

        tv.tv_sec = 0;
        tv.tv_usec = 100 * 1000; // 100 ms

        int select_res = select(1 + self->initial_socket_fd, &rfds, NULL, NULL, &tv);
        if(select_res > 0) {
            socklen_t sock_len = 0;
            self->initial_client_fd = accept(self->initial_socket_fd, (struct sockaddr*)&remote_addr, &sock_len);
            if(self->initial_client_fd == -1) {
                fprintf(stderr, "gsr error: gsr_kms_client_init: accept failed on socket, error: %s\n", strerror(errno));
                goto err;
            }
            break;
        } else {
            int status = 0;
            int wait_result = waitpid(self->kms_server_pid, &status, WNOHANG);
            if(wait_result != 0) {
                int exit_code = -1;
                if(WIFEXITED(status))
                    exit_code = WEXITSTATUS(status);
                fprintf(stderr, "gsr error: gsr_kms_client_init: kms server died or never started, exit code: %d\n", exit_code);
                self->kms_server_pid = -1;
                if(exit_code != 0)
                    result = exit_code;
                goto err;
            }
        }
    }
    fprintf(stderr, "gsr info: gsr_kms_client_init: server connected\n");

    fprintf(stderr, "gsr info: replacing file-backed unix domain socket with socketpair\n");
    if(gsr_kms_client_replace_connection(self) != 0)
        goto err;

    cleanup_socket(self, false);
    fprintf(stderr, "gsr info: using socketpair\n");

    return 0;

    err:
    gsr_kms_client_deinit(self);
    return result;
}

void cleanup_socket(gsr_kms_client *self, bool kill_server) {
    if(self->initial_client_fd > 0) {
        close(self->initial_client_fd);
        self->initial_client_fd = -1;
    }

    if(self->initial_socket_fd > 0) {
        close(self->initial_socket_fd);
        self->initial_socket_fd = -1;
    }

    if(kill_server) {
        for(int i = 0; i < 2; ++i) {
            if(self->socket_pair[i] > 0) {
                close(self->socket_pair[i]);
                self->socket_pair[i] = -1;
            }
        }
    }

    if(kill_server && self->kms_server_pid > 0) {
        kill(self->kms_server_pid, SIGKILL);
        //int status;
        //waitpid(self->kms_server_pid, &status, 0);
        self->kms_server_pid = -1;
    }

    if(self->initial_socket_path[0] != '\0') {
        remove(self->initial_socket_path);
        self->initial_socket_path[0] = '\0';
    }
}

void gsr_kms_client_deinit(gsr_kms_client *self) {
    cleanup_socket(self, true);
}

int gsr_kms_client_replace_connection(gsr_kms_client *self) {
    gsr_kms_response response;
    response.version = 0;
    response.result = KMS_RESULT_FAILED_TO_SEND;
    response.err_msg[0] = '\0';

    gsr_kms_request request;
    request.version = GSR_KMS_PROTOCOL_VERSION;
    request.type = KMS_REQUEST_TYPE_REPLACE_CONNECTION;
    request.new_connection_fd = self->socket_pair[GSR_SOCKET_PAIR_REMOTE];
    if(send_msg_to_server(self->initial_client_fd, &request) == -1) {
        fprintf(stderr, "gsr error: gsr_kms_client_replace_connection: failed to send request message to server\n");
        return -1;
    }

    const int recv_res = recv_msg_from_server(self->kms_server_pid, self->socket_pair[GSR_SOCKET_PAIR_LOCAL], &response);
    if(recv_res == 0) {
        fprintf(stderr, "gsr warning: gsr_kms_client_replace_connection: kms server shut down\n");
        return -1;
    } else if(recv_res == -1) {
        fprintf(stderr, "gsr error: gsr_kms_client_replace_connection: failed to receive response\n");
        return -1;
    }

    if(response.version != GSR_KMS_PROTOCOL_VERSION) {
        fprintf(stderr, "gsr error: gsr_kms_client_replace_connection: expected gsr-kms-server protocol version to be %u, but it's %u. please reinstall gpu screen recorder\n", GSR_KMS_PROTOCOL_VERSION, response.version);
        /*close_fds(response);*/
        return -1;
    }

    return 0;
}

int gsr_kms_client_get_kms(gsr_kms_client *self, gsr_kms_response *response) {
    response->version = 0;
    response->result = KMS_RESULT_FAILED_TO_SEND;
    response->err_msg[0] = '\0';

    gsr_kms_request request;
    request.version = GSR_KMS_PROTOCOL_VERSION;
    request.type = KMS_REQUEST_TYPE_GET_KMS;
    request.new_connection_fd = 0;
    if(send_msg_to_server(self->socket_pair[GSR_SOCKET_PAIR_LOCAL], &request) == -1) {
        fprintf(stderr, "gsr error: gsr_kms_client_get_kms: failed to send request message to server\n");
        strcpy(response->err_msg, "failed to send");
        return -1;
    }

    const int recv_res = recv_msg_from_server(self->kms_server_pid, self->socket_pair[GSR_SOCKET_PAIR_LOCAL], response);
    if(recv_res == 0) {
        fprintf(stderr, "gsr warning: gsr_kms_client_get_kms: kms server shut down\n");
        strcpy(response->err_msg, "failed to receive");
        return -1;
    } else if(recv_res == -1) {
        fprintf(stderr, "gsr error: gsr_kms_client_get_kms: failed to receive response\n");
        strcpy(response->err_msg, "failed to receive");
        return -1;
    }

    if(response->version != GSR_KMS_PROTOCOL_VERSION) {
        fprintf(stderr, "gsr error: gsr_kms_client_get_kms: expected gsr-kms-server protocol version to be %u, but it's %u. please reinstall gpu screen recorder\n", GSR_KMS_PROTOCOL_VERSION, response->version);
        /*close_fds(response);*/
        strcpy(response->err_msg, "mismatching protocol version");
        return -1;
    }

    return 0;
}
