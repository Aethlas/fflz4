#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <getopt.h>

#include "./lib/cJSON/cJSON.h"
#include "./lib/lz4/lz4.h"

#define FAILED(with_usage, ...) \
    do { \
        fprintf(stderr, __VA_ARGS__); \
        \
        if (with_usage) { \
            usage(); \
        } \
        \
        exit(EXIT_FAILURE); \
    } \
    while (0)

static void usage(void)
{
    printf("Usage fflz4:\n");
    printf("-u\tSession urls\n");
    printf("-t\tSession titles\n");
    printf("-c\tActive title & url. Can be limited with -u or -t\n");
    printf("-b\tBookmarks title & url saved in toolbar. Can be limited with -u or -t\n");
    printf("-s\tRaw session json\n");
    printf("-d\tRaw bookmarks json\n");
    printf("-p\t(Optional) Provide path to firefox profile. Default: /home/<user>/.mozilla/firefox/<xxxxxx.default>/\n");
}

enum {
    FLAG_P = 1 << 0,
    FLAG_U = 1 << 1,
    FLAG_T = 1 << 2,
    FLAG_C = 1 << 3,
    FLAG_B = 1 << 4,
    FLAG_S = 1 << 5,
    FLAG_D = 1 << 6
};

static const char *SESSION = "sessionstore-backups";
static const char *BOOKMARK = "bookmarkbackups";
static const char *TABS = "recovery.jsonlz4";

time_t get_last_modified(const char *path);
char *extract_jsonlz4(const char *path);

char *get_firefox_profile(void);
char *get_bookmark_file(const char *path);
char *get_file_content(const char *path, const char *rw, const size_t offset, size_t *size);

void get_tabs(const char *path, const unsigned int flag);
void print_tabs(const cJSON *json, const unsigned int flag);

void get_bookmarks(const char *path, const unsigned int flag);
void print_bookmarks(const cJSON *child, const cJSON *children, const unsigned int flag);

int main(int argc, char **argv)
{
    if (argc < 2) FAILED(1, "");

    char *profile = NULL, *bookmark = NULL;
    unsigned int flag = 0x0;
    int c = 0;

    while ((c = getopt(argc, argv, "p:utcbsd")) != -1) {
        switch (c) {
            case 'p': /* profile path */
                flag |= FLAG_P;
                profile = strdup(optarg);
                break;
            case 'u': /* url */
                flag |= FLAG_U;
                break;
            case 't': /* title */
                flag |= FLAG_T;
                break;
            case 'c': /* current session */
                flag |= FLAG_C;
                break;
            case 'b': /* bookmark */
                flag |= FLAG_B;
                break;
            case 's': /* session json */
                flag |= FLAG_S;
                break;
            case 'd': /* bookmark json */
                flag |= FLAG_D;
                break;
            case '?':
                if (optopt == 'p')
                    FAILED(1, "");
                break;
            default:
                break;
        }
    }

    if (!profile)
        profile = get_firefox_profile();

    if (flag & (FLAG_U | FLAG_T | FLAG_C | FLAG_S))
        get_tabs(profile, flag);

    if (flag & (FLAG_B | FLAG_D)) {
        bookmark = get_bookmark_file(profile);
        get_bookmarks(bookmark, flag);
        free(bookmark);
    }

    free(profile);
    return EXIT_SUCCESS;
}

time_t get_last_modified(const char *path)
{
    struct stat attr;
    if (stat(path, &attr) == 0)
        return attr.st_mtime;
    return 0;
}

char *extract_jsonlz4(const char *path)
{
    size_t SIZE = 0;
    unsigned int dst_size;
    const int hdr_size = sizeof(unsigned int);
    char *source = NULL, *result = NULL;

    /* read file */
    /* skip "mozLz40\0" header */
    source = get_file_content(path, "rb", 8, &SIZE);

    memcpy(&dst_size, source, 4);
    if (dst_size > INT_MAX) FAILED(0, "Invalid size in header: 0x%x\n", dst_size);

    result = calloc(dst_size+1, sizeof(char));
    if (!result) FAILED(0, "Not enough memory\n");

    int osize = LZ4_decompress_safe(source + hdr_size, result, SIZE - hdr_size, dst_size);
    if (osize < 0) FAILED(0, "Corrupt input at byte %d\n", -osize);

    free(source);
    return result;
}

char *get_firefox_profile(void)
{
    char *home = NULL, *source = NULL, *result_path = NULL;
    char path[4096] = {0}, profile[256] = {0};
    size_t SIZE = 0;
    int len = 0;

    home = getenv("HOME");
    if (!home) FAILED(0, "Get $HOME failed\n");

    sprintf(path, "%s/.mozilla/firefox/profiles.ini", home);
    source = get_file_content(path, "r", 0, &SIZE);

    /* get firefox profile */
    for (const char *tok = strtok(source, "\n"); tok && *tok; tok = strtok(NULL, "\n")) {
        if (strstr(tok, "Path")) {
            char *r = strstr(tok, "=");
            size_t pos = r-tok+1, len = strlen(tok);
            size_t j = 0;
            for (size_t i = pos; i < len; ++i) {
                profile[j++] = tok[i];
            }
            profile[j] = '\0';
            break;
        }
    }

    if (strlen(profile))
        len = asprintf(&result_path, "%s/.mozilla/firefox/%s/", home, profile);

    if (!len) FAILED(0, "Get profile path failed\n");

    free(source);
    return result_path;
}

char *get_bookmark_file(const char *path)
{
    char *buf = NULL, *result = NULL, *p = NULL;
    int len = 0;
    char filep[4096] = {0};
    time_t t1 = 0, t2 = -1;
    struct dirent *entry;
    DIR *dp;

    len = path[strlen(path)-1] == '/'? asprintf(&buf, "%s%s/", path, BOOKMARK) : asprintf(&buf, "%s/%s/", path, BOOKMARK);
    if (!len) FAILED(0, "Not enough memory\n");

    dp = opendir(buf);
    if (!dp) FAILED(0, "Open directory failed at: %s\n", buf);

    while ((entry = readdir(dp))) {
        if (strstr(entry->d_name, "bookmarks")) {
            sprintf(filep, "%s%s", buf, entry->d_name);
            t1 = get_last_modified(filep);
            if (t2 == -1) {
                t2 = t1;
                p = filep;
            }
            else if (t1 > t2) {
                t2 = t1;
                p = filep;
            }
        }
    }

    result = strdup(p);
    if (!result) FAILED(0, "Get bookmark file failed\n");

    closedir(dp);
    free(buf);
    return result;
}

char *get_file_content(const char *path, const char *rw, const size_t offset, size_t *size)
{
    FILE *fptr;
    size_t SIZE = 0, sz = 0;
    char *result = NULL;

    fptr = fopen(path, rw);
    if (!fptr) FAILED(0, "File not found at: %s", path);

    fseek(fptr, 0, SEEK_END);
    SIZE = ftell(fptr);
    fseek(fptr, offset, SEEK_SET);
    SIZE -= offset;

    result = calloc(SIZE+1, sizeof(char));
    if (!result) FAILED(0, "Not enough memory\n");

    sz = fread(result, 1, SIZE, fptr);
    if (sz != SIZE) FAILED(0, "File reading failed\n");

    fclose(fptr);

    *size = SIZE;
    return result;
}

void get_tabs(const char *path, const unsigned int flag)
{
    cJSON *json = NULL;
    char *buf = NULL, *result = NULL;
    int len = 0;

    len = path[strlen(path)-1] == '/' ? asprintf(&buf, "%s%s/%s", path, SESSION, TABS) : asprintf(&buf, "%s/%s/%s", path, SESSION, TABS);
    if (!len) FAILED(0, "Not enough memory\n");

    result = extract_jsonlz4(buf);
    if (!result) FAILED(0, "Failed to read jsonlz4 at: %s\n", buf);

    json = cJSON_Parse(result);
    if (!json) goto cleanup;

    if (flag & FLAG_S) {
        char *js = cJSON_Print(json);
        fprintf(stdout, "%s\n", js);
        free(js);
        goto cleanup;
    }

    const cJSON *windows = cJSON_GetObjectItemCaseSensitive(json, "windows");
    if (!cJSON_IsArray(windows)) goto cleanup;

    print_tabs(windows, flag);

cleanup:
    if (json) cJSON_Delete(json);
    free(buf);
    free(result);
}

void print_tabs(const cJSON *json, const unsigned int flag)
{
    const cJSON *active_title = NULL, *active_url = NULL;
    const cJSON *child = NULL;
    long lastAccessed = 0;

    cJSON_ArrayForEach(child, json) {
        const cJSON *tabs = cJSON_GetObjectItemCaseSensitive(child, "tabs");
        if (!cJSON_IsArray(tabs)) break;

        const cJSON *tab = NULL;
        cJSON_ArrayForEach(tab, tabs) {
            const cJSON *accessed = cJSON_GetObjectItemCaseSensitive(tab, "lastAccessed");
            const cJSON *index = cJSON_GetObjectItemCaseSensitive(tab, "index");
            const cJSON *entries = cJSON_GetObjectItemCaseSensitive(tab, "entries");
            if (!cJSON_IsArray(entries) || cJSON_IsNull(index)) break;

            const cJSON *entry = cJSON_GetArrayItem(entries, index->valueint-1);
            if (!cJSON_IsObject(entry)) break;

            const cJSON *title = cJSON_GetObjectItemCaseSensitive(entry, "title");
            const cJSON *url = cJSON_GetObjectItemCaseSensitive(entry, "url");

            if (!cJSON_IsNull(accessed)) {
                char *ac = cJSON_Print(accessed);
                long tmp = strtol(ac, NULL, 10);
                if (lastAccessed < tmp) {
                    lastAccessed = tmp;
                    active_title = cJSON_GetObjectItemCaseSensitive(entry, "title");
                    active_url = cJSON_GetObjectItemCaseSensitive(entry, "url");
                }
                free(ac);
            }

            if ((flag & (FLAG_T | FLAG_U)) == (FLAG_T | FLAG_U)) {
                if (cJSON_IsString(title) && cJSON_IsString(url))
                    fprintf(stdout, "%s\n%s\n\n", title->valuestring, url->valuestring);
            } else {
                if ((flag & FLAG_T) && !(flag & FLAG_C) && cJSON_IsString(title))
                    fprintf(stdout, "%s\n", title->valuestring);

                if ((flag & FLAG_U) && !(flag & FLAG_C) && cJSON_IsString(url))
                    fprintf(stdout, "%s\n", url->valuestring);
            }
        }
    }

    if (!cJSON_IsString(active_title) && !cJSON_IsString(active_url))
        return;

    if ((flag & (FLAG_C | FLAG_T)) == (FLAG_C | FLAG_T))
        fprintf(stdout, "%s\n", active_title->valuestring);
    else if ((flag & (FLAG_C | FLAG_U)) == (FLAG_C | FLAG_U))
        fprintf(stdout, "%s\n", active_url->valuestring);
    else if (flag & FLAG_C)
        fprintf(stdout, "%s\n%s\n", active_title->valuestring, active_url->valuestring);
}

void get_bookmarks(const char *path, const unsigned int flag)
{
    cJSON *json = NULL;
    char *result = NULL;

    result = extract_jsonlz4(path);
    if (!result) FAILED(0, "Failed to read jsonlz4 at: %s\n", path);

    json = cJSON_Parse(result);
    if (!json) goto cleanup;

    if (flag & FLAG_D) {
        char *js = cJSON_Print(json);
        fprintf(stdout, "%s\n", js);
        free(js);
        goto cleanup;
    }

    const cJSON *children = cJSON_GetObjectItemCaseSensitive(json, "children");
    if (!cJSON_IsArray(children)) goto cleanup;

    const cJSON *child = NULL;

    /* get bookmarks toolbar */
    cJSON_ArrayForEach(child, children) {
        const cJSON *root = cJSON_GetObjectItemCaseSensitive(child, "root");
        if (cJSON_IsString(root)) {
            if (!strcmp(root->valuestring, "toolbarFolder")) {
                children = cJSON_GetObjectItemCaseSensitive(child, "children");
                break;
            }
        }
    }

    if (cJSON_IsArray(children))
        print_bookmarks(child, children, flag);
    else
        FAILED(0, "Failed to get bookmarks toolbar\n");

cleanup:
    if (json) cJSON_Delete(json);
    free(result);
}

void print_bookmarks(const cJSON *child, const cJSON *children, const unsigned int flag)
{
    cJSON_ArrayForEach(child, children) {
        const cJSON *url = cJSON_GetObjectItemCaseSensitive(child, "uri");
        const cJSON *deep = cJSON_GetObjectItemCaseSensitive(child, "children");

        if (cJSON_IsString(url)) {
            const cJSON *title = cJSON_GetObjectItemCaseSensitive(child, "title");

            if (((flag & (FLAG_T | FLAG_U)) == 0) && cJSON_IsString(title))
                fprintf(stdout, "%s\n%s\n\n", title->valuestring, url->valuestring);
            if (flag & FLAG_U)
                fprintf(stdout, "%s\n", url->valuestring);
            if ((flag & FLAG_T) && cJSON_IsString(title))
                fprintf(stdout, "%s\n", title->valuestring);
        }
        else if (cJSON_IsArray(deep)) {
            print_bookmarks(child, deep, flag);
        }
    }
}
