#pragma comment(lib, "ws2_32.lib")
#define _CRT_SECURE_NO_WARNINGS
#define _WINSOCK_DEPRECATED_NO_WARNINGS

#include <winsock2.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

#define PORT              12345
#define BUF_SIZE          4096
#define MAX_FILENAME      512

#define CMD_COUNT_SPACES  0x01
#define CMD_DOWNLOAD      0x02
#define CMD_UPLOAD        0x03

#define STATUS_OK         0x00
#define STATUS_ERR        0x01

#define LOG_FILE          "server_log.txt"

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

// Отправка ответа по статусу
static int send_status(SOCKET s, unsigned char status, const char* msg)
{
    char hdr[3];
    unsigned short mlen = (unsigned short)strlen(msg);
    hdr[0] = (char)status;
    put_u16(hdr + 1, mlen);
    if (send_all(s, hdr, 3) != 0) return -1;
    if (mlen > 0 && send_all(s, msg, (int)mlen) != 0) return -1;
    return 0;
}

// Чтение заголовка запроса
static int recv_request_header(SOCKET s, char* filename_out)
{
    char cmd_byte;
    if (recv_all(s, &cmd_byte, 1) != 0) return -1;

    char len_buf[2];
    if (recv_all(s, len_buf, 2) != 0) return -1;
    unsigned short name_len = get_u16(len_buf);

    if (name_len == 0 || name_len >= MAX_FILENAME)
    {
        log_write("  [!] Invalid filename length: %u", name_len);
        return -1;
    }
    if (recv_all(s, filename_out, (int)name_len) != 0) return -1;
    filename_out[name_len] = '\0';

    return (unsigned char)cmd_byte;
}

//подсчет пробелов
static void handle_count_spaces(SOCKET s, const char* filename)
{
    FILE* f = fopen(filename, "r");
    if (!f)
    {
        char msg[MAX_FILENAME + 64];
        snprintf(msg, sizeof(msg), "Cannot open file: %s", filename);
        send_status(s, STATUS_ERR, msg);
        log_write("  [!] %s", msg);
        return;
    }
    long long spaces = 0;
    int c;
    while ((c = fgetc(f)) != EOF)
        if (c == ' ') spaces++;
    fclose(f);

    char msg[64];
    snprintf(msg, sizeof(msg), "%lld", spaces);
    send_status(s, STATUS_OK, msg);
    log_write("  [+] /spaces \"%s\" -> %lld space(s)", filename, spaces);
}
//сервер отдает файл клиенту
static void handle_download(SOCKET s, const char* filename)
{
    FILE* f = fopen(filename, "rb");
    if (!f)
    {
        char msg[MAX_FILENAME + 64];
        snprintf(msg, sizeof(msg), "Cannot open file: %s", filename);
        send_status(s, STATUS_ERR, msg);
        log_write("  [!] /get \"%s\" -> NOT FOUND", filename);
        return;
    }
    fseek(f, 0, SEEK_END);
    long long file_size = (long long)ftell(f);
    fseek(f, 0, SEEK_SET);
    if (send_status(s, STATUS_OK, "") != 0) { fclose(f); return; }
    char size_buf[8];
    put_i64(size_buf, file_size);
    if (send_all(s, size_buf, 8) != 0)
    {
        fclose(f);
        log_write("  [!] Failed to send file size for \"%s\"", filename);
        return;
    }
    char buf[BUF_SIZE];
    long long sent_total = 0;
    int bytes_read;
    while ((bytes_read = (int)fread(buf, 1, sizeof(buf), f)) > 0)
    {
        if (send_all(s, buf, bytes_read) != 0)
        {
            log_write("  [!] Connection lost while sending \"%s\"", filename);
            fclose(f);
            return;
        }
        sent_total += bytes_read;
    }
    fclose(f);
    log_write("  [+] /get \"%s\" -> sent %lld byte(s)", filename, sent_total);
}
//сервер получает файл от клиента
static void handle_upload(SOCKET s, const char* filename)
{
    char size_buf[8];
    if (recv_all(s, size_buf, 8) != 0)
    {
        send_status(s, STATUS_ERR, "Failed to receive file size");
        return;
    }
    long long file_size = get_i64(size_buf);
    if (file_size < 0)
    {
        send_status(s, STATUS_ERR, "Invalid file size");
        return;
    }
    FILE* f = fopen(filename, "wb");
    if (!f)
    {
        char msg[MAX_FILENAME + 64];
        snprintf(msg, sizeof(msg), "Cannot create file on server: %s", filename);
        send_status(s, STATUS_ERR, msg);
        log_write("  [!] %s", msg);
        char drain[BUF_SIZE];
        long long left = file_size;
        while (left > 0)
        {
            int chunk = (left > BUF_SIZE) ? BUF_SIZE : (int)left;
            if (recv_all(s, drain, chunk) != 0) break;
            left -= chunk;
        }
        return;
    }
    char buf[BUF_SIZE];
    long long left = file_size;
    int ok = 1;
    while (left > 0)
    {
        int chunk = (left > BUF_SIZE) ? BUF_SIZE : (int)left;
        if (recv_all(s, buf, chunk) != 0)
        {
            log_write("  [!] Connection lost while receiving \"%s\"", filename);
            ok = 0; break;
        }
        if ((int)fwrite(buf, 1, chunk, f) != chunk)
        {
            log_write("  [!] Write error for \"%s\" (disk full?)", filename);
            ok = 0; break;
        }
        left -= chunk;
    }
    fclose(f);

    if (ok)
    {
        char msg[64];
        snprintf(msg, sizeof(msg), "%lld", file_size);
        send_status(s, STATUS_OK, msg);
        log_write("  [+] /put \"%s\" -> received %lld byte(s)", filename, file_size);
    }
    else
    {
        remove(filename);
        send_status(s, STATUS_ERR, "Upload incomplete, file removed");
    }
}

static void handle_client(SOCKET s, const char* client_ip)
{
    char filename[MAX_FILENAME];
    int cmd = recv_request_header(s, filename);
    if (cmd < 0)
    {
        log_write("  [!] Bad request from %s", client_ip);
        return;
    }
    const char* cmd_name =
        (cmd == CMD_COUNT_SPACES) ? "/spaces" :
        (cmd == CMD_DOWNLOAD) ? "/get" :
        (cmd == CMD_UPLOAD) ? "/put" : "unknown";

    log_write("[>] %s  client=%s  file=\"%s\"", cmd_name, client_ip, filename);
    switch (cmd)
    {
    case CMD_COUNT_SPACES: handle_count_spaces(s, filename); break;
    case CMD_DOWNLOAD:     handle_download(s, filename);     break;
    case CMD_UPLOAD:       handle_upload(s, filename);       break;
    default:
    {
        char msg[32];
        snprintf(msg, sizeof(msg), "Unknown command: %d", cmd);
        send_status(s, STATUS_ERR, msg);
        log_write("  [!] %s from %s", msg, client_ip);
    }
    }
}

int main(void)
{
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
    {
        printf("WSAStartup failed\n");
        return 1;
    }
    SOCKET srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv == INVALID_SOCKET)
    {
        printf("socket() failed: %d\n", WSAGetLastError());
        WSACleanup(); return 1;
    }
    int opt = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(PORT);
    if (bind(srv, (struct sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR)
    {
        printf("bind() failed: %d\n", WSAGetLastError());
        closesocket(srv); WSACleanup(); return 1;
    }
    if (listen(srv, 5) == SOCKET_ERROR)
    {
        printf("listen() failed: %d\n", WSAGetLastError());
        closesocket(srv); WSACleanup(); return 1;
    }
    log_write("================================================");
    log_write("  Server started on port %d", PORT);
    log_write("  Log file: %s  (same folder as .exe)", LOG_FILE);
    log_write("  Files are resolved relative to .exe location");
    log_write("  Commands: /spaces  /get  /put");
    log_write("================================================");
    for (;;)
    {
        struct sockaddr_in cli_addr;
        int cli_len = sizeof(cli_addr);
        SOCKET cli = accept(srv, (struct sockaddr*)&cli_addr, &cli_len);
        if (cli == INVALID_SOCKET)
        {
            log_write("accept() failed: %d", WSAGetLastError());
            continue;
        }
        const char* ip = inet_ntoa(cli_addr.sin_addr);
        log_write("[+] Connected: %s", ip);
        handle_client(cli, ip);
        log_write("[-] Disconnected: %s", ip);
        closesocket(cli);
    }
    closesocket(srv);
    WSACleanup();
    return 0;
}