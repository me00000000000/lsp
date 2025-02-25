#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <pwd.h>
#include <grp.h>
#include <time.h>
#include <string.h>
#include <errno.h>
#include <limits.h>

#define COLOR_RESET "\033[0m"
#define COLOR_GREEN "\033[1;32m"
#define COLOR_ORANGE "\033[38;5;208m"
#define COLOR_RED "\033[1;31m"
#define COLOR_DIR "\033[1;34m"
#define COLOR_FILE "\033[0m"
#define COLOR_DARK_GREY "\033[90m"
#define COLOR_GREY "\033[37m"

typedef struct {
    char *name;
    char fullpath[PATH_MAX];
    mode_t mode;
    uid_t uid;
    gid_t gid;
    off_t size;
    time_t mtime;
    int is_dir;
} FileEntry;

off_t get_directory_size(const char *path) {
    off_t total = 0;
    DIR *d = opendir(path);
    if (!d) { return 0; }
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        char full[PATH_MAX];
        snprintf(full, PATH_MAX, "%s/%s", path, e->d_name);
        struct stat st;
        if (stat(full, &st) == 0) {
            if (S_ISDIR(st.st_mode))
                total += get_directory_size(full);
            else
                total += st.st_size;
        }
    }
    closedir(d);
    return total;
}

void human_readable_size(off_t size, char *buf, size_t bufsize) {
    const char *units[] = {"B", "KB", "MB", "GB", "TB"};
    int i = 0;
    double dsize = size;
    while (dsize >= 1024 && i < 4) { dsize /= 1024; i++; }
    snprintf(buf, bufsize, "%.1f %s", dsize, units[i]);
}

void get_permission_string(mode_t mode, char *str) {
    str[0] = S_ISDIR(mode) ? 'd' : '-';
    str[1] = (mode & S_IRUSR) ? 'r' : '-';
    str[2] = (mode & S_IWUSR) ? 'w' : '-';
    str[3] = (mode & S_IXUSR) ? 'x' : '-';
    str[4] = (mode & S_IRGRP) ? 'r' : '-';
    str[5] = (mode & S_IWGRP) ? 'w' : '-';
    str[6] = (mode & S_IXGRP) ? 'x' : '-';
    str[7] = (mode & S_IROTH) ? 'r' : '-';
    str[8] = (mode & S_IWOTH) ? 'w' : '-';
    str[9] = (mode & S_IXOTH) ? 'x' : '-';
    str[10] = '\0';
}

int cmp_entries(const void *a, const void *b) {
    FileEntry *fa = *(FileEntry **)a;
    FileEntry *fb = *(FileEntry **)b;
    if (fa->is_dir != fb->is_dir)
        return fb->is_dir - fa->is_dir;
    if (fa->mtime < fb->mtime)
        return 1;
    else if (fa->mtime > fb->mtime)
        return -1;
    return 0;
}

void time_ago(time_t mtime, char *buf, size_t bufsize) {
    time_t now = time(NULL);
    double seconds = difftime(now, mtime);
    
    if (seconds < 60) {
        snprintf(buf, bufsize, "%.0fs ago", seconds);
    } else if (seconds < 3600) {
        snprintf(buf, bufsize, "%.0fm ago", seconds / 60);
    } else if (seconds < 86400) {
        snprintf(buf, bufsize, "%.0fh ago", seconds / 3600);  
    } else if (seconds < 2592000) {
        snprintf(buf, bufsize, "%.0fd ago", seconds / 86400);
    } else if (seconds < 31536000) {
        snprintf(buf, bufsize, "%.0fmo ago", seconds / 2592000);
    } else {
        snprintf(buf, bufsize, "%.0fy ago", seconds / 31536000);
    }
}

int main(int argc, char *argv[]) {
    int show_hidden = 0;
    const char *dirpath = ".";

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0) {
            show_hidden = 1;
        } else {
            dirpath = argv[i];
        }
    }

    DIR *d = opendir(dirpath);
    if (!d) { return EXIT_FAILURE; }
    size_t cap = 16, count = 0;
    FileEntry **entries = malloc(cap * sizeof(FileEntry *));
    if (!entries) { closedir(d); return EXIT_FAILURE; }
    struct dirent *dp;
    while ((dp = readdir(d)) != NULL) {
        if (!show_hidden && dp->d_name[0] == '.') {
            continue;
        }
        FileEntry *fe = malloc(sizeof(FileEntry));
        if (!fe) { continue; }
        fe->name = strdup(dp->d_name);
        snprintf(fe->fullpath, PATH_MAX, "%s/%s", dirpath, dp->d_name);
        struct stat st;
        if (stat(fe->fullpath, &st) < 0) {
            free(fe->name);
            free(fe);
            continue;
        }
        fe->mode = st.st_mode;
        fe->uid = st.st_uid;
        fe->gid = st.st_gid;
        fe->mtime = st.st_mtime;
        fe->is_dir = S_ISDIR(st.st_mode);
        fe->size = fe->is_dir ? get_directory_size(fe->fullpath) : st.st_size;
        if (count >= cap) {
            cap *= 2;
            FileEntry **tmp = realloc(entries, cap * sizeof(FileEntry *));
            if (!tmp) { free(fe->name); free(fe); break; }
            entries = tmp;
        }
        entries[count++] = fe;
    }
    closedir(d);
    qsort(entries, count, sizeof(FileEntry *), cmp_entries);

    int max_perm = 0, max_user = 0, max_size = 0, max_date = 0, max_name = 0;
    for (size_t i = 0; i < count; i++) {
        FileEntry *fe = entries[i];
        char perms[11]; get_permission_string(fe->mode, perms);
        int l = strlen(perms); if (l > max_perm) max_perm = l;
        struct passwd *pwd = getpwuid(fe->uid);
        struct group *grp = getgrgid(fe->gid);
        char usergroup[64];
        snprintf(usergroup, sizeof(usergroup), "%s:%s", pwd ? pwd->pw_name : "unknown", grp ? grp->gr_name : "unknown");
        l = strlen(usergroup); if (l > max_user) max_user = l;
        char size_str[32]; human_readable_size(fe->size, size_str, sizeof(size_str));
        l = strlen(size_str); if (l > max_size) max_size = l;
        char time_str[32]; time_ago(fe->mtime, time_str, sizeof(time_str));
        l = strlen(time_str); if (l > max_date) max_date = l;
        l = strlen(fe->name); if (l > max_name) max_name = l;
    }
    
    for (size_t i = 0; i < count; i++) {
        FileEntry *fe = entries[i];
        char perms[11]; get_permission_string(fe->mode, perms);
        struct passwd *pwd = getpwuid(fe->uid);
        struct group *grp = getgrgid(fe->gid);
        char usergroup[64];
        snprintf(usergroup, sizeof(usergroup), "%s:%s", pwd ? pwd->pw_name : "unknown", grp ? grp->gr_name : "unknown");
        char size_str[32]; human_readable_size(fe->size, size_str, sizeof(size_str));
        const char *size_color = "";
        if (strstr(size_str, "KB") != NULL)
            size_color = COLOR_GREEN;
        else if (strstr(size_str, "MB") != NULL)
            size_color = COLOR_ORANGE;
        else if (strstr(size_str, "GB") != NULL)
            size_color = COLOR_RED;

        char time_str[32];
        time_ago(fe->mtime, time_str, sizeof(time_str));

        double seconds = difftime(time(NULL), fe->mtime);
        const char *date_color = "";
        if (seconds >= 31536000)
            date_color = COLOR_DARK_GREY;
        else if (seconds >= 2592000)
            date_color = COLOR_GREY;

        printf("%-*s  %-*s  %s%-*s%s  %s%-*s%s  %s%-*s%s\n",
            max_perm, perms,
            max_user, usergroup,
            size_color, max_size, size_str, COLOR_RESET,
            date_color, max_date, time_str, COLOR_RESET,
            (fe->is_dir ? COLOR_DIR : COLOR_FILE), max_name, fe->name, COLOR_RESET);
        free(fe->name);
        free(fe);
    }
    free(entries);
    return EXIT_SUCCESS;
}
