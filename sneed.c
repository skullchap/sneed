/*
 * SNEED SERVER - v0.0.1 - C89 ANSI POSIX1 concurrent file server.
 *
 * MIT LICENSE
 *
 * Copyright <2023> <skullchap> <https://github.com/skullchap/>
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software,
 * and to permit persons to whom the Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all copies or
 * substantial portions of the Software.
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define SERVER_PORT 8888
#define BUFFER_SIZE 8192
#define TIMEOUT_SEC 5

void
print_usage(char *program_name)
{
        fprintf(stderr, "Usage: %s [-d directory] [-i initialfile] [-p port]\n", program_name);
}

typedef struct task {
        int          fd, active;
        FILE        *file_stream;
        struct task *prev;
        struct task *next;
} task_t;

task_t *head = NULL;

task_t *
create_task(int client_sock);
void
add_task(task_t *task);
void
remove_task(task_t *task);
void
schedule_tasks(void);
void
serve_client(task_t *task);
int
create_server_socket(void);
int
sanitize_path(const char *path);
int
is_get_request(const char *request);
int
get_path(const char *request, char *path_buffer, size_t buffer_size);
int
send_directory_listing(int fd, const char *directory_path, const char *relative_path);
ssize_t
write_all(int fd, const void *buffer, size_t count);
int
usleep_milliseconds(long milliseconds);
void
print_usage(char *program_name);
void
parse_arguments(int argc, char *argv[]);

static char *serving_directory = NULL;
static char *initial_file = NULL;
static int   server_port = SERVER_PORT;

int
main(int argc, char *argv[])
{
        int                server_sock, client_sock, flags;
        struct sockaddr_in client_addr;
        socklen_t          client_addr_len = sizeof(client_addr);
        task_t            *task, *new_task;
        fd_set             read_fds;
        int                max_fd;
        struct timeval     timeout;

        parse_arguments(argc, argv);
        signal(SIGPIPE, SIG_IGN);

        server_sock = create_server_socket();
        if (server_sock < 0) {
                perror("Failed to create server socket");
                exit(EXIT_FAILURE);
        }

        timeout.tv_sec = TIMEOUT_SEC;
        timeout.tv_usec = 0;

        while (1) {
                FD_ZERO(&read_fds);
                FD_SET(server_sock, &read_fds);
                max_fd = server_sock;

                for (task = head; task; task = task->next) {
                        if (task->active) {
                                FD_SET(task->fd, &read_fds);
                                if (task->fd > max_fd) {
                                        max_fd = task->fd;
                                }
                        }
                }

                if (select(max_fd + 1, &read_fds, NULL, NULL, &timeout) < 0) {
                        perror("select error");
                        continue;
                }

                if (FD_ISSET(server_sock, &read_fds)) {
                        client_sock = accept(server_sock, (struct sockaddr *)&client_addr, &client_addr_len);
                        if (client_sock >= 0) {
                                flags = fcntl(client_sock, F_GETFL, 0);
                                if (flags < 0 || fcntl(client_sock, F_SETFL, flags | O_NONBLOCK) < 0) {
                                        perror("Error setting O_NONBLOCK on client socket");
                                        close(client_sock);
                                        continue;
                                }
                                new_task = create_task(client_sock);
                                if (new_task) {
                                        add_task(new_task);
                                }
                        } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
                                perror("Error accepting connection");
                        }
                }

                for (task = head; task; task = task->next) {
                        if (task->active && FD_ISSET(task->fd, &read_fds)) {
                                serve_client(task);
                        }
                }

                schedule_tasks();
        }

        close(server_sock);
        return 0;
}

task_t *
create_task(int client_sock)
{
        task_t *new_task = (task_t *)malloc(sizeof(task_t));
        if (!new_task) {
                perror("Error allocating memory for new task");
                close(client_sock);
                return NULL;
        }

        new_task->active = 1;
        new_task->fd = client_sock;
        new_task->file_stream = NULL;
        return new_task;
}

void
add_task(task_t *task)
{
        task->next = head;
        task->prev = NULL;
        if (head != NULL) {
                head->prev = task;
        }
        head = task;
}

void
remove_task(task_t *task)
{
        if (task->prev != NULL) {
                task->prev->next = task->next;
        } else {
                head = task->next;
        }
        if (task->next != NULL) {
                task->next->prev = task->prev;
        }
        free(task);
}

void
schedule_tasks(void)
{
        task_t *task = head;
        task_t *next_task;

        while (task != NULL) {
                next_task = task->next;
                if (task->active) {
                        serve_client(task);
                } else {
                        usleep_milliseconds(100);
                        if (task->file_stream) {
                                fclose(task->file_stream);
                                task->file_stream = NULL;
                        }
                        if (task->fd >= 0) {
                                close(task->fd);
                                task->fd = -1;
                        }
                        remove_task(task);
                }
                task = next_task;
        }
}

int
is_get_request(const char *request)
{
        return strncmp(request, "GET ", 4) == 0;
}

int
get_path(const char *request, char *path_buffer, size_t buffer_size)
{
        const char *start, *end;
        start = strstr(request, "GET ");
        if (start) {
                start += 4;
                end = strstr(start, " ");
                if (end && (size_t)(end - start) < buffer_size) {
                        size_t path_length = end - start;
                        strncpy(path_buffer, start, path_length);
                        path_buffer[path_length] = '\0';
                        return 0;
                }
        }
        return -1;
}

void
serve_client(task_t *task)
{
        char req_buffer[1024],
            path[PATH_MAX], full_path[PATH_MAX],
            header[128], buffer[BUFFER_SIZE];
        char       *p;
        int         header_length, path_extracted;
        size_t      dir_len, remaining;
        ssize_t     bytes_read, bytes_written, write_result;
        long        file_size;
        struct stat path_stat;

        /* Initial read and path extraction */
        if (task->file_stream == NULL) {
                bytes_read = read(task->fd, req_buffer, sizeof(req_buffer) - 1);
                if (bytes_read <= 0) {
                        if (errno != EAGAIN && errno != EWOULDBLOCK) {
                                perror("Error reading from client");
                                task->active = 0;
                        }
                        return;
                }

                req_buffer[bytes_read] = '\0';

                path_extracted = get_path(req_buffer, path, sizeof(path));
                if (path_extracted != 0 || sanitize_path(path) != 0) {
                        write(task->fd, "HTTP/1.1 400 Bad Request\r\n\r\n", 28);
                        task->active = 0;
                        return;
                }

                strncpy(full_path, serving_directory, sizeof(full_path));
                full_path[sizeof(full_path) - 1] = '\0';
                dir_len = strlen(full_path);
                remaining = sizeof(full_path) - dir_len - 1;

                /* Add a slash if the directory does not already end with one */
                if (full_path[dir_len - 1] != '/' && remaining > 0) {
                        full_path[dir_len++] = '/';
                        full_path[dir_len] = '\0';
                        --remaining;
                }

                p = (strcmp(path, "/") == 0 && initial_file) ? initial_file : path;
                if (remaining > strlen(p)) {
                        strncat(full_path, p, remaining);
                } else {
                        write(task->fd, "HTTP/1.1 414 URI Too Long\r\n\r\n", 29);
                        task->active = 0;
                        return;
                }

                /* Check if the path is a directory or a file */
                if (stat(full_path, &path_stat) == 0) {
                        if (S_ISDIR(path_stat.st_mode)) {
                                p = (strcmp(path, "/") == 0) ? "/" : path;
                                if (send_directory_listing(task->fd, full_path, p) == -1) {
                                        write(task->fd, "HTTP/1.1 500 Internal Server Error\r\n\r\n", 38);
                                }
                                task->active = 0;
                                return;
                        }
                } else {
                        perror("Error stating the path");
                        puts(path);
                        write(task->fd, "HTTP/1.1 404 Not Found\r\n\r\n", 26);
                        task->active = 0;
                        return;
                }

                task->file_stream = fopen(full_path, "rb");
                if (!task->file_stream) {
                        perror("Error opening file");
                        puts(path);
                        write(task->fd, "HTTP/1.1 404 Not Found\r\n\r\n", 26);
                        task->active = 0;
                        return;
                }

                /* Seek to the end of the file to determine its size */
                if (fseek(task->file_stream, 0, SEEK_END) != 0) {
                        perror("Error seeking in file");
                        write(task->fd, "HTTP/1.1 500 Internal Server Error\r\n\r\n", 38);
                        fclose(task->file_stream);
                        task->active = 0;
                        return;
                }

                file_size = ftell(task->file_stream);
                if (file_size == -1) {
                        perror("Error telling the file size");
                        write(task->fd, "HTTP/1.1 500 Internal Server Error\r\n\r\n", 36);
                        fclose(task->file_stream);
                        task->active = 0;
                        return;
                }

                /* Seek back to the beginning of the file */
                if (fseek(task->file_stream, 0, SEEK_SET) != 0) {
                        perror("Error seeking in file");
                        write(task->fd, "HTTP/1.1 500 Internal Server Error\r\n\r\n", 36);
                        fclose(task->file_stream);
                        task->active = 0;
                        return;
                }

                /* Prepare the HTTP response headers with Content-Length */
                header_length = sprintf(header, "HTTP/1.1 200 OK\r\n"
                                                "Content-Length: %ld\r\n"
                                                "\r\n",
                                        file_size);

                if (header_length > sizeof(header)) {
                        fprintf(stderr, "Header buffer too small\n");
                        write(task->fd, "HTTP/1.1 500 Internal Server Error\r\n\r\n", 36);
                        fclose(task->file_stream);
                        task->active = 0;
                        return;
                }

                /* Send the HTTP response headers */
                write(task->fd, header, header_length);
        }

        /* Read from file and write to socket. */
        while ((bytes_read = fread(buffer, 1, BUFFER_SIZE, task->file_stream)) > 0) {
                bytes_written = write_all(task->fd, buffer, bytes_read);
                if (bytes_written < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                                fseek(task->file_stream, -(bytes_read), SEEK_CUR);
                                break;
                        } else if (errno == EPIPE) {
                                task->active = 0;
                        } else {
                                perror("Error writing to client");
                                task->active = 0;
                        }
                        break;
                }
                if (bytes_written < bytes_read) {
                        continue;
                }
                if (bytes_written == bytes_read) {
                        break;
                }
        }

        /* Clean up on end of file or error. */
        if (feof(task->file_stream) || ferror(task->file_stream)) {
                fclose(task->file_stream);
                task->file_stream = NULL;
                task->active = 0;
        }
}

int
create_server_socket(void)
{
        int                sockfd;
        struct sockaddr_in server_addr;

        sockfd = socket(AF_INET, SOCK_STREAM, 0);
        if (sockfd < 0) {
                perror("Error creating socket");
                return -1;
        }

        if (fcntl(sockfd, F_SETFL, O_NONBLOCK) < 0) {
                perror("Error setting O_NONBLOCK on server socket");
                close(sockfd);
                return -1;
        }

        memset(&server_addr, 0, sizeof(server_addr));
        server_addr.sin_family = AF_INET;
        server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
        server_addr.sin_port = htons(server_port);

        if (bind(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
                perror("Error binding socket to server address");
                close(sockfd);
                return -1;
        }

        if (listen(sockfd, SOMAXCONN) < 0) {
                perror("Error listening on socket");
                close(sockfd);
                return -1;
        }

        return sockfd;
}

int
sanitize_path(const char *path)
{
        if (strstr(path, "../") != NULL || strstr(path, "/..") != NULL || strcmp(path, "..") == 0) {
                return -1;
        }
        return 0;
}

int
send_directory_listing(int fd, const char *directory_path, const char *relative_path)
{
        DIR           *dir;
        struct dirent *entry;
        struct stat    entry_stat;
        char          *html_response, *new_response;
        size_t         html_size = 1024;
        size_t         bytes_used, entry_len;
        char           header[128];
        int            header_len;
        char           full_path[PATH_MAX];
        char           link[PATH_MAX];

        dir = opendir(directory_path);
        if (!dir) {
                perror("Error opening directory");
                return -1;
        }

        html_response = malloc(html_size);
        if (!html_response) {
                perror("Error allocating memory");
                closedir(dir);
                return -1;
        }

        strcpy(html_response, "<!DOCTYPE html><html><head><title>Directory Listing</title></head><body>");
        strcat(html_response, "<h1>Directory Listing for ");
        strcat(html_response, relative_path);
        strcat(html_response, "</h1><ul>");
        bytes_used = strlen(html_response);

        while ((entry = readdir(dir)) != NULL) {
                if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
                        continue;
                }

                sprintf(full_path, "%s/%s", directory_path, entry->d_name);
                if (stat(full_path, &entry_stat) == -1) {
                        continue;
                }

                if (S_ISDIR(entry_stat.st_mode)) {
                        sprintf(link, "%s%s/", relative_path, entry->d_name);
                } else {
                        sprintf(link, "%s%s", relative_path, entry->d_name);
                }

                entry_len = strlen(link) * 2 + 50; /* Approximation for the entry */

                if (bytes_used + entry_len > html_size) {
                        html_size *= 2;
                        new_response = realloc(html_response, html_size);
                        if (!new_response) {
                                perror("Error reallocating memory");
                                free(html_response);
                                closedir(dir);
                                return -1;
                        }
                        html_response = new_response;
                }

                strcat(html_response, "<li><a href=\"");
                strcat(html_response, link);
                strcat(html_response, "\">");
                strcat(html_response, entry->d_name);
                if (S_ISDIR(entry_stat.st_mode)) {
                        strcat(html_response, "/"); /* Append '/' to display name if it's a directory */
                }
                strcat(html_response, "</a></li>");
                bytes_used += entry_len;
        }

        strcat(html_response, "</ul></body></html>");

        header_len = sprintf(header, "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: %lu\r\n\r\n", (unsigned long)strlen(html_response));
        if (write_all(fd, header, header_len) < 0) {
                perror("Error writing header to file descriptor");
                free(html_response);
                closedir(dir);
                return -1;
        }
        if (write_all(fd, html_response, strlen(html_response)) < 0) {
                perror("Error writing HTML response to file descriptor");
                free(html_response);
                closedir(dir);
                return -1;
        }

        free(html_response);
        closedir(dir);

        return 0;
}

ssize_t
write_all(int fd, const void *buffer, size_t count)
{
        size_t         bytes_written = 0;
        const uint8_t *buf_ptr = buffer;
        while (bytes_written < count) {
                ssize_t res = write(fd, buf_ptr + bytes_written, count - bytes_written);
                if (res < 0) {
                        if (errno == EINTR)
                                continue;
                        return -1;
                }
                if (res == 0) {
                        break;
                }
                bytes_written += (size_t)res;
        }
        return (ssize_t)bytes_written;
}

int
usleep_milliseconds(long milliseconds)
{
        struct timespec req, rem;
        req.tv_sec = milliseconds / 1000;
        req.tv_nsec = (milliseconds % 1000) * 1000000L;
        while (nanosleep(&req, &rem) == -1) {
                if (errno == EINTR) {
                        /* Interrupted by a signal, place remaining time back into req and retry */
                        req = rem;
                } else {
                        /* An actual error occurred */
                        return -1;
                }
        }
        return 0;
}

void
parse_arguments(int argc, char *argv[])
{
        char *arg, *next_arg;
        int   i;
        char  full_path[PATH_MAX];

        for (i = 1; i < argc; i++) {
                arg = argv[i];

                if (arg[0] == '-' && strlen(arg) == 2) {
                        if (i + 1 >= argc) {
                                fprintf(stderr, "Option requires an argument: %s\n", arg);
                                print_usage(argv[0]);
                                exit(EXIT_FAILURE);
                        }

                        next_arg = argv[i + 1];
                        switch (arg[1]) {
                        case 'd':
                                if (realpath(next_arg, full_path) == NULL) {
                                        perror("Error resolving full path of directory");
                                        exit(EXIT_FAILURE);
                                }
                                serving_directory = malloc(strlen(full_path) + 1);
                                if (serving_directory == NULL) {
                                        perror("Error allocating memory for directory path");
                                        exit(EXIT_FAILURE);
                                }
                                strcpy(serving_directory, full_path);
                                i++;
                                break;
                        case 'i':
                                initial_file = next_arg;
                                i++;
                                break;
                        case 'p':
                                server_port = atoi(next_arg);
                                if (server_port <= 0 || server_port > 65535) {
                                        fprintf(stderr, "Invalid port number: %s\n", next_arg);
                                        exit(EXIT_FAILURE);
                                }
                                i++;
                                break;
                        default:
                                fprintf(stderr, "Unknown option: %c\n", arg[1]);
                                print_usage(argv[0]);
                                exit(EXIT_FAILURE);
                        }
                } else {
                        fprintf(stderr, "Invalid argument format: %s\n", arg);
                        print_usage(argv[0]);
                        exit(EXIT_FAILURE);
                }
        }

        if (serving_directory == NULL) {
                if (getcwd(full_path, sizeof(full_path)) == NULL) {
                        perror("getcwd() error");
                        exit(EXIT_FAILURE);
                }
                serving_directory = malloc(strlen(full_path) + 1);
                if (serving_directory == NULL) {
                        perror("Error allocating memory for current working directory");
                        exit(EXIT_FAILURE);
                }
                strcpy(serving_directory, full_path);
        }

        printf("Serving directory: %s\n", serving_directory ? serving_directory : "(current)");
        printf("Initial file: %s\n", initial_file ? initial_file : "(none)");
        printf("Server port: %d\n", server_port);
}
