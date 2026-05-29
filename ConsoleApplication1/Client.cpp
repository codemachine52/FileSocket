#pragma comment(lib, "ws2_32.lib")
#define _CRT_SECURE_NO_WARNINGS
#define _WINSOCK_DEPRECATED_NO_WARNINGS

#include <winsock2.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

#define SERVER_IP         "10.37.216.75"
#define PORT              12345
#define BUF_SIZE          4096
#define MAX_FILENAME      512
#define MAX_LINE          1100 

#define CMD_COUNT_SPACES  0x01
#define CMD_DOWNLOAD      0x02
#define CMD_UPLOAD        0x03

#define STATUS_OK         0x00
#define STATUS_ERR        0x01

#define LOG_FILE          "client_log.txt"

static void log_write(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");

    FILE* lf = fopen(LOG_FILE, "a");
    if (!lf) return;

    time_t t = time(NULL);
    struct tm* tm = localtime(&t);
    fprintf(lf, "[%04d-%02d-%02d %02d:%02d:%02d] ",
        tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
        tm->tm_hour, tm->tm_min, tm->tm_sec);

    va_start(ap, fmt);
    vfprintf(lf, fmt, ap);
    va_end(ap);
    fprintf(lf, "\n");
    fclose(lf);
}

static int send_all(SOCKET s, const char* buf, int n)
{
    int sent = 0;
    while (sent < n)
    {
        int r = send(s, buf + sent, n - sent, 0);
        if (r <= 0) return -1;
        sent += r;
    }
    return 0;
}

static int recv_all(SOCKET s, char* buf, int n)
{
    int got = 0;
    while (got < n)
    {
        int r = recv(s, buf + got, n - got, 0);
        if (r <= 0) return -1;
        got += r;
    }
    return 0;
}

static void put_u16(char* p, unsigned short v)
{
    p[0] = (char)((v >> 8) & 0xFF);
    p[1] = (char)(v & 0xFF);
}
static unsigned short get_u16(const char* p)
{
    return (unsigned short)(((unsigned char)p[0] << 8) | (unsigned char)p[1]);
}
static void put_i64(char* p, long long v)
{
    for (int i = 7; i >= 0; --i) { p[i] = (char)(v & 0xFF); v >>= 8; }
}
static long long get_i64(const char* p)
{
    long long v = 0;
    for (int i = 0; i < 8; ++i) v = (v << 8) | (unsigned char)p[i];
    return v;
}

static SOCKET connect_to_server(void)
{
    SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET)
    {
        log_write("  [!] socket() failed: %d", WSAGetLastError());
        return INVALID_SOCKET;
    }
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr(SERVER_IP);
    addr.sin_port = htons(PORT);

    if (connect(s, (struct sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR)
    {
        log_write("  [!] Cannot connect to %s:%d  (WinSock error %d)",
            SERVER_IP, PORT, WSAGetLastError());
        closesocket(s);
        return INVALID_SOCKET;
    }
    return s;
}

static int send_request_header(SOCKET s, unsigned char cmd, const char* filename)
{
    char hdr[3];
    unsigned short name_len = (unsigned short)strlen(filename);
    hdr[0] = (char)cmd;
    put_u16(hdr + 1, name_len);
    if (send_all(s, hdr, 3) != 0) return -1;
    if (send_all(s, filename, (int)name_len) != 0) return -1;
    return 0;
}

static int recv_status(SOCKET s, char* msg_out, int msg_out_size)
{
    char hdr[3];
    if (recv_all(s, hdr, 3) != 0) return -1;
    unsigned char  status = (unsigned char)hdr[0];
    unsigned short mlen = get_u16(hdr + 1);

    msg_out[0] = '\0';
    if (mlen > 0)
    {
        int to_read = (mlen < (unsigned short)(msg_out_size - 1))
            ? (int)mlen : msg_out_size - 1;
        if (recv_all(s, msg_out, to_read) != 0) return -1;
        msg_out[to_read] = '\0';
        int extra = (int)mlen - to_read;
        char trash[64];
        while (extra > 0)
        {
            int chunk = (extra > 64) ? 64 : extra;
            if (recv_all(s, trash, chunk) != 0) return -1;
            extra -= chunk;
        }
    }
    return (int)status;
}

static void print_help(void)
{
    printf("\n");
    printf("  Available commands:\n");
    printf("  ----------------------------------------------------------\n");
    printf("  /help                           - show this help\n");
    printf("  /spaces <server_file>           - count spaces in file\n");
    printf("                                    on server\n");
    printf("  /get <server_file> <local_file> - download file from\n");
    printf("                                    server to client\n");
    printf("  /put <local_file> <server_file> - upload file from\n");
    printf("                                    client to server\n");
    printf("  /exit                           - quit\n");
    printf("  ----------------------------------------------------------\n");
    printf("  File path examples:\n");
    printf("    Relative (from .exe folder): test.txt\n");
    printf("    Absolute: C:\\Users\\user\\Desktop\\test.txt\n");
    printf("\n");
}

static void cmd_spaces(const char* server_file)
{
    if (server_file[0] == '\0')
    {
        printf("  Usage: /spaces <server_file>\n");
        return;
    }

    log_write("  >> /spaces \"%s\"", server_file);

    SOCKET s = connect_to_server();
    if (s == INVALID_SOCKET) return;

    if (send_request_header(s, CMD_COUNT_SPACES, server_file) != 0)
    {
        log_write("  [!] Failed to send request.");
        closesocket(s); return;
    }

    char msg[256];
    int status = recv_status(s, msg, sizeof(msg));
    closesocket(s);

    if (status < 0)
        log_write("  [!] Connection error while reading response.");
    else if (status == STATUS_OK)
        log_write("  [+] Spaces in \"%s\": %s", server_file, msg);
    else
        log_write("  [!] Server error: %s", msg);
}

static void cmd_get(const char* server_file, const char* local_file)
{
    if (server_file[0] == '\0' || local_file[0] == '\0')
    {
        printf("  Usage: /get <server_file> <local_file>\n");
        printf("  Example: /get test.txt C:\\Users\\me\\Desktop\\test.txt\n");
        return;
    }

    log_write("  >> /get  server:\"%s\"  local:\"%s\"", server_file, local_file);

    SOCKET s = connect_to_server();
    if (s == INVALID_SOCKET) return;

    if (send_request_header(s, CMD_DOWNLOAD, server_file) != 0)
    {
        log_write("  [!] Failed to send request.");
        closesocket(s); return;
    }

    char msg[256];
    int status = recv_status(s, msg, sizeof(msg));
    if (status < 0)
    {
        log_write("  [!] Connection error while reading status.");
        closesocket(s); return;
    }
    if (status == STATUS_ERR)
    {
        log_write("  [!] Server error: %s", msg);
        closesocket(s); return;
    }
    char size_buf[8];
    if (recv_all(s, size_buf, 8) != 0)
    {
        log_write("  [!] Failed to receive file size.");
        closesocket(s); return;
    }
    long long file_size = get_i64(size_buf);
    printf("  File size on server: %lld byte(s)\n", file_size);
    FILE* f = fopen(local_file, "wb");
    if (!f)
    {
        log_write("  [!] Cannot create local file \"%s\".", local_file);
        log_write("      Make sure the DIRECTORY exists and the filename is correct.");
        log_write("      Example: C:\\Users\\user\\Desktop\\downloaded.txt");
        char drain[BUF_SIZE];
        long long left = file_size;
        while (left > 0)
        {
            int chunk = (left > BUF_SIZE) ? BUF_SIZE : (int)left;
            if (recv_all(s, drain, chunk) != 0) break;
            left -= chunk;
        }
        closesocket(s); return;
    }
    char buf[BUF_SIZE];
    long long left = file_size;
    int error = 0;
    while (left > 0)
    {
        int chunk = (left > BUF_SIZE) ? BUF_SIZE : (int)left;
        if (recv_all(s, buf, chunk) != 0)
        {
            log_write("  [!] Connection lost during download.");
            error = 1; break;
        }
        if ((int)fwrite(buf, 1, chunk, f) != chunk)
        {
            log_write("  [!] Write error (disk full?).");
            error = 1; break;
        }
        left -= chunk;
    }
    fclose(f);
    closesocket(s);

    if (error)
    {
        remove(local_file);
        log_write("  [!] Download failed, incomplete file removed.");
    }
    else
    {
        log_write("  [+] Downloaded \"%s\" -> \"%s\" (%lld bytes)",
            server_file, local_file, file_size);
    }
}

static void cmd_put(const char* local_file, const char* server_file)
{
    if (local_file[0] == '\0' || server_file[0] == '\0')
    {
        printf("  Usage: /put <local_file> <server_file>\n");
        printf("  Example: /put C:\\Users\\me\\Desktop\\hello.txt hello.txt\n");
        return;
    }
    FILE* f = fopen(local_file, "rb");
    if (!f)
    {
        log_write("  [!] Cannot open local file \"%s\".", local_file);
        log_write("      Check path. Relative paths resolve from Client.exe folder.");
        return;
    }
    fseek(f, 0, SEEK_END);
    long long file_size = (long long)ftell(f);
    fseek(f, 0, SEEK_SET);

    log_write("  >> /put  local:\"%s\"  server:\"%s\"  size:%lld",
        local_file, server_file, file_size);

    SOCKET s = connect_to_server();
    if (s == INVALID_SOCKET) { fclose(f); return; }

    if (send_request_header(s, CMD_UPLOAD, server_file) != 0)
    {
        log_write("  [!] Failed to send request header.");
        fclose(f); closesocket(s); return;
    }

    char size_buf[8];
    put_i64(size_buf, file_size);
    if (send_all(s, size_buf, 8) != 0)
    {
        log_write("  [!] Failed to send file size.");
        fclose(f); closesocket(s); return;
    }
    char buf[BUF_SIZE];
    long long sent_total = 0;
    int bytes_read, error = 0;
    printf("  Sending %lld byte(s)...\n", file_size);
    while ((bytes_read = (int)fread(buf, 1, sizeof(buf), f)) > 0)
    {
        if (send_all(s, buf, bytes_read) != 0)
        {
            log_write("  [!] Connection lost during upload.");
            error = 1; break;
        }
        sent_total += bytes_read;
    }
    fclose(f);

    if (error) { closesocket(s); return; }
    char msg[256];
    int status = recv_status(s, msg, sizeof(msg));
    closesocket(s);

    if (status < 0)
        log_write("  [!] Connection error reading server response.");
    else if (status == STATUS_OK)
        log_write("  [+] Uploaded \"%s\" -> server:\"%s\" (%lld bytes)",
            local_file, server_file, sent_total);
    else
        log_write("  [!] Server error: %s", msg);
}

static int parse_line(char* line,
    char* tok0, int t0sz,
    char* tok1, int t1sz,
    char* tok2, int t2sz)
{
    tok0[0] = tok1[0] = tok2[0] = '\0';
    line[strcspn(line, "\r\n")] = '\0';
    char* p = line;
    while (*p == ' ') ++p;
    if (*p == '\0') return 0;
    int i = 0;
    while (*p && *p != ' ' && i < t0sz - 1) tok0[i++] = *p++;
    tok0[i] = '\0';
    if (*p == '\0') return 1;
    while (*p == ' ') ++p;
    if (*p == '\0') return 1;
    i = 0;
    while (*p && *p != ' ' && i < t1sz - 1) tok1[i++] = *p++;
    tok1[i] = '\0';
    if (*p == '\0') return 2;
    while (*p == ' ') ++p;
    if (*p == '\0') return 2;

    i = 0;
    while (*p && i < t2sz - 1) tok2[i++] = *p++;
    tok2[i] = '\0';

    return 3;
}

int main(void)
{
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
    {
        printf("WSAStartup failed\n");
        return 1;
    }
    printf("  File Client  |  server %s:%d\n", SERVER_IP, PORT);
    printf("  Log: %s  (same folder as Client.exe)\n", LOG_FILE);
    printf("  Type /help for commands\n");
    log_write("=== Client session started ===");

    char line[MAX_LINE];
    char cmd[32], arg1[MAX_FILENAME], arg2[MAX_FILENAME];

    for (;;)
    {
        printf("> ");
        fflush(stdout);

        if (!fgets(line, sizeof(line), stdin)) break;

        int tokens = parse_line(line, cmd, sizeof(cmd),
            arg1, sizeof(arg1),
            arg2, sizeof(arg2));
        if (tokens == 0) continue;

        if (strcmp(cmd, "/help") == 0)
        {
            print_help();
        }
        else if (strcmp(cmd, "/spaces") == 0)
        {
            cmd_spaces(arg1);
        }
        else if (strcmp(cmd, "/get") == 0)
        {
            cmd_get(arg1, arg2);
        }
        else if (strcmp(cmd, "/put") == 0)
        {
            cmd_put(arg1, arg2);
        }
        else if (strcmp(cmd, "/exit") == 0 || strcmp(cmd, "/quit") == 0)
        {
            log_write("=== Client session ended ===");
            printf("Goodbye!\n");
            break;
        }
        else
        {
            printf("  Unknown command: %s  (type /help)\n", cmd);
        }
        printf("\n");
    }

    WSACleanup();
    return 0;
}