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
#include <unistd.h>
#include <glob.h>
#include <fcntl.h>
#include <pthread.h>

#define COLOR_RESET "\033[0m"
#define COLOR_GREEN "\033[1;32m"
#define COLOR_ORANGE "\033[38;5;208m"
#define COLOR_RED "\033[1;31m"
#define COLOR_DIR "\033[1;34m"
#define COLOR_FILE "\033[0m"
#define COLOR_DARK_GREY "\033[90m"
#define COLOR_GREY "\033[37m"
#define COLOR_SYMLINK "\033[1;36m"
#define COLOR_YELLOW "\033[1;33m"
#define COLOR_LINKTARGET "\033[37m"

#define BUF_SIZE 32
#define THREAD_THRESHOLD 10

typedef struct {
    char *name;
    char fullpath[PATH_MAX];
    mode_t mode;
    uid_t uid;
    gid_t gid;
    off_t size;
    time_t mtime;
    int is_dir;
    int is_symlink;
    char *link_target;
    ino_t inode;
    nlink_t nlink;
} FileEntry;

off_t get_directory_size(const char *path) {
    off_t total = 0;
    DIR *d = opendir(path);
    if (!d)
        return 0;
    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;
        char full[PATH_MAX];
        snprintf(full, PATH_MAX, "%s/%s", path, entry->d_name);
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
    while (dsize >= 1024 && i < 4) {
        dsize /= 1024;
        i++;
    }
    if (i == 0)
        snprintf(buf, bufsize, "%lld %s", (long long)size, units[i]);
    else
        snprintf(buf, bufsize, "%.1f %s", dsize, units[i]);
}

void get_permission_string(mode_t mode, char *str) {
    if (S_ISDIR(mode))
        str[0] = 'd';
    else if (S_ISLNK(mode))
        str[0] = 'l';
    else if (S_ISCHR(mode))
        str[0] = 'c';
    else if (S_ISBLK(mode))
        str[0] = 'b';
    else if (S_ISFIFO(mode))
        str[0] = 'p';
    else if (S_ISSOCK(mode))
        str[0] = 's';
    else
        str[0] = '-';
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

void time_ago(time_t mtime, time_t now, char *buf, size_t bufsize) {
    double seconds = difftime(now, mtime);
    if (seconds < 60)
        snprintf(buf, bufsize, "%.0fs ago", seconds);
    else if (seconds < 3600)
        snprintf(buf, bufsize, "%.0fm ago", seconds / 60);
    else if (seconds < 86400)
        snprintf(buf, bufsize, "%.0fh ago", seconds / 3600);
    else if (seconds < 2592000)
        snprintf(buf, bufsize, "%.0fd ago", seconds / 86400);
    else if (seconds < 31536000)
        snprintf(buf, bufsize, "%.0fmo ago", seconds / 2592000);
    else
        snprintf(buf, bufsize, "%.0fy ago", seconds / 31536000);
}

void print_entries(FileEntry **entries, size_t count, int show_inode) {
    int max_perm = 0, max_user = 0, max_size = 0, max_date = 0, max_inode = 0, max_nlink = 0;
    size_t max_name = 0;
    char size_buf[BUF_SIZE], time_buf[BUF_SIZE];
    time_t now = time(NULL);
    for (size_t i = 0; i < count; i++) {
        FileEntry *fe = entries[i];
        char perms[11];
        get_permission_string(fe->mode, perms);
        int len = strlen(perms);
        if (len > max_perm)
            max_perm = len;
        struct passwd *pwd = getpwuid(fe->uid);
        struct group *grp = getgrgid(fe->gid);
        char usergroup[64];
        snprintf(usergroup, sizeof(usergroup), "%s:%s", pwd ? pwd->pw_name : "unknown", grp ? grp->gr_name : "unknown");
        len = strlen(usergroup);
        if (len > max_user)
            max_user = len;
        human_readable_size(fe->size, size_buf, sizeof(size_buf));
        len = strlen(size_buf);
        if (len > max_size)
            max_size = len;
        time_ago(fe->mtime, now, time_buf, sizeof(time_buf));
        len = strlen(time_buf);
        if (len > max_date)
            max_date = len;
        len = strlen(fe->name);
        if (len > max_name)
            max_name = len;
        if (show_inode) {
            char inode_str[BUF_SIZE];
            snprintf(inode_str, sizeof(inode_str), "%lu", (unsigned long)fe->inode);
            len = strlen(inode_str);
            if (len > max_inode)
                max_inode = len;
            snprintf(inode_str, sizeof(inode_str), "%lu", (unsigned long)fe->nlink);
            len = strlen(inode_str);
            if (len > max_nlink)
                max_nlink = len;
        }
    }
    char line[1024];
    for (size_t i = 0; i < count; i++) {
        FileEntry *fe = entries[i];
        char perms[11];
        get_permission_string(fe->mode, perms);
        struct passwd *pwd = getpwuid(fe->uid);
        struct group *grp = getgrgid(fe->gid);
        char usergroup[64];
        snprintf(usergroup, sizeof(usergroup), "%s:%s", pwd ? pwd->pw_name : "unknown", grp ? grp->gr_name : "unknown");
        human_readable_size(fe->size, size_buf, sizeof(size_buf));
        const char *size_color = "";
        if (strstr(size_buf, "KB"))
            size_color = COLOR_GREEN;
        else if (strstr(size_buf, "MB"))
            size_color = COLOR_ORANGE;
        else if (strstr(size_buf, "GB"))
            size_color = COLOR_RED;
        time_ago(fe->mtime, now, time_buf, sizeof(time_buf));
        double seconds = difftime(now, fe->mtime);
        const char *date_color = "";
        if (seconds >= 31536000)
            date_color = COLOR_DARK_GREY;
        else if (seconds >= 2592000)
            date_color = COLOR_GREY;
        const char *name_color;
        if (fe->is_symlink)
            name_color = COLOR_SYMLINK;
        else if (fe->is_dir)
            name_color = COLOR_DIR;
        else if ((fe->mode & S_IXUSR) || (fe->mode & S_IXGRP) || (fe->mode & S_IXOTH))
            name_color = COLOR_GREEN;
        else
            name_color = COLOR_FILE;
        int pos = 0;
        if (show_inode) {
            char inode_str[BUF_SIZE];
            snprintf(inode_str, sizeof(inode_str), "%-*lu", max_inode, (unsigned long)fe->inode);
            pos += snprintf(line+pos, sizeof(line)-pos, "%s  ", inode_str);
            snprintf(inode_str, sizeof(inode_str), "%-*lu", max_nlink, (unsigned long)fe->nlink);
            pos += snprintf(line+pos, sizeof(line)-pos, "%s  ", inode_str);
        }
        pos += snprintf(line+pos, sizeof(line)-pos, "%-*s  %-*s  %s%-*s%s  %s%-*s%s  ",
                        max_perm, perms,
                        max_user, usergroup,
                        size_color, max_size, size_buf, COLOR_RESET,
                        date_color, max_date, time_buf, COLOR_RESET);
        if (fe->is_symlink && fe->link_target) {
            struct stat st_target;
            int stat_ret = stat(fe->fullpath, &st_target);
            const char *target_color = COLOR_LINKTARGET;
            int is_char = 0, is_block = 0;
            if (stat_ret == 0) {
                if (S_ISDIR(st_target.st_mode))
                    target_color = COLOR_DIR;
                else if (S_ISCHR(st_target.st_mode))
                    is_char = 1;
                else if (S_ISBLK(st_target.st_mode))
                    is_block = 1;
            }
            pos += snprintf(line+pos, sizeof(line)-pos, "%s%s%s -> %s%s%s", name_color, fe->name, COLOR_RESET, target_color, fe->link_target, COLOR_RESET);
            if (is_char)
                pos += snprintf(line+pos, sizeof(line)-pos, "%s*%s", COLOR_RED, COLOR_RESET);
            else if (is_block)
                pos += snprintf(line+pos, sizeof(line)-pos, "%s#%s", COLOR_YELLOW, COLOR_RESET);
        } else {
            pos += snprintf(line+pos, sizeof(line)-pos, "%s%s%s", name_color, fe->name, COLOR_RESET);
        }
        if (S_ISCHR(fe->mode))
            pos += snprintf(line+pos, sizeof(line)-pos, "%s*%s", COLOR_RED, COLOR_RESET);
        else if (S_ISBLK(fe->mode))
            pos += snprintf(line+pos, sizeof(line)-pos, "%s#%s", COLOR_YELLOW, COLOR_RESET);
        puts(line);
    }
}

typedef struct {
    int dirfd;
    char dirpath[PATH_MAX];
    struct dirent *dentry;
    FileEntry **result;
} ThreadArg;

void *process_entry_thread(void *arg) {
    ThreadArg *ta = arg;
    char full[PATH_MAX];
    snprintf(full, PATH_MAX, "%s/%s", ta->dirpath, ta->dentry->d_name);
    struct stat st;
    if (fstatat(ta->dirfd, ta->dentry->d_name, &st, AT_SYMLINK_NOFOLLOW) < 0)
        *ta->result = NULL;
    else {
        FileEntry *fe = malloc(sizeof(FileEntry));
        if (fe) {
            fe->name = strdup(ta->dentry->d_name);
            strncpy(fe->fullpath, full, PATH_MAX);
            fe->mode = st.st_mode;
            fe->uid = st.st_uid;
            fe->gid = st.st_gid;
            fe->mtime = st.st_mtime;
            fe->inode = st.st_ino;
            fe->nlink = st.st_nlink;
            fe->is_dir = S_ISDIR(st.st_mode);
            if (S_ISLNK(st.st_mode)) {
                fe->is_symlink = 1;
                char target[PATH_MAX];
                ssize_t len = readlink(full, target, sizeof(target) - 1);
                if (len != -1) {
                    target[len] = '\0';
                    fe->link_target = strdup(target);
                } else
                    fe->link_target = strdup("unreadable");
                fe->size = st.st_size;
            } else {
                fe->is_symlink = 0;
                fe->link_target = NULL;
                fe->size = fe->is_dir ? get_directory_size(full) : st.st_size;
            }
            *ta->result = fe;
        } else
            *ta->result = NULL;
    }
    return NULL;
}

FileEntry *populate_file_entry(const char *name, const char *fullpath, const struct stat *st) {
    FileEntry *fe = malloc(sizeof(FileEntry));
    if (!fe)
        return NULL;
    fe->name = strdup(name);
    strncpy(fe->fullpath, fullpath, PATH_MAX);
    fe->mode = st->st_mode;
    fe->uid = st->st_uid;
    fe->gid = st->st_gid;
    fe->mtime = st->st_mtime;
    fe->inode = st->st_ino;
    fe->nlink = st->st_nlink;
    fe->is_dir = S_ISDIR(st->st_mode);
    if (S_ISLNK(st->st_mode)) {
        fe->is_symlink = 1;
        char target[PATH_MAX];
        ssize_t len = readlink(fullpath, target, sizeof(target) - 1);
        if (len != -1) {
            target[len] = '\0';
            fe->link_target = strdup(target);
        } else
            fe->link_target = strdup("unreadable");
        fe->size = st->st_size;
    } else {
        fe->is_symlink = 0;
        fe->link_target = NULL;
        fe->size = fe->is_dir ? get_directory_size(fullpath) : st->st_size;
    }
    return fe;
}

FileEntry* create_file_entry(const char *filepath) {
    struct stat st;
    if (fstatat(AT_FDCWD, filepath, &st, AT_SYMLINK_NOFOLLOW) < 0)
        return NULL;
    return populate_file_entry(filepath, filepath, &st);
}

void process_file_collect(const char *filepath, FileEntry ***files, size_t *count, size_t *cap) {
    FileEntry *fe = create_file_entry(filepath);
    if (!fe)
        return;
    if (*count >= *cap) {
        *cap *= 2;
        FileEntry **tmp = realloc(*files, *cap * sizeof(FileEntry *));
        if (!tmp) {
            free(fe->name);
            free(fe);
            return;
        }
        *files = tmp;
    }
    (*files)[(*count)++] = fe;
}

void process_directory(const char *dirpath, int show_hidden, int print_header, int show_inode) {
    struct dirent **namelist;
    int n = scandir(dirpath, &namelist, NULL, alphasort);
    if (n < 0)
        return;
    if (print_header)
        printf("%s:\n", dirpath);
    int dirfd = open(dirpath, O_RDONLY | O_DIRECTORY);
    if (dirfd < 0) {
        for (int i = 0; i < n; i++) {
            if (namelist[i]) free(namelist[i]);
        }
        free(namelist);
        return;
    }
    FileEntry **entries = malloc(n * sizeof(FileEntry *));
    size_t count = 0;
    pthread_t *threads = NULL;
    ThreadArg *targs = NULL;
    int use_threads = (n >= THREAD_THRESHOLD);
    if (use_threads) {
        threads = malloc(n * sizeof(pthread_t));
        targs = malloc(n * sizeof(ThreadArg));
    }
    for (int i = 0; i < n; i++) {
        if (!show_hidden && namelist[i]->d_name[0] == '.') {
            free(namelist[i]);
            namelist[i] = NULL;
            continue;
        }
        if (use_threads) {
            targs[count].dirfd = dirfd;
            strncpy(targs[count].dirpath, dirpath, PATH_MAX);
            targs[count].dentry = namelist[i];
            targs[count].result = &entries[count];
            pthread_create(&threads[count], NULL, process_entry_thread, &targs[count]);
        } else {
            char full[PATH_MAX];
            snprintf(full, PATH_MAX, "%s/%s", dirpath, namelist[i]->d_name);
            struct stat st;
            if (fstatat(dirfd, namelist[i]->d_name, &st, AT_SYMLINK_NOFOLLOW) < 0) {
                free(namelist[i]);
                namelist[i] = NULL;
                continue;
            }
            FileEntry *fe = populate_file_entry(namelist[i]->d_name, full, &st);
            entries[count] = fe;
        }
        count++;
    }
    if (use_threads) {
        for (size_t i = 0; i < count; i++)
            pthread_join(threads[i], NULL);
        free(threads);
        free(targs);
    }
    for (int i = 0; i < n; i++) {
        if (namelist[i])
            free(namelist[i]);
    }
    free(namelist);
    close(dirfd);
    qsort(entries, count, sizeof(FileEntry *), cmp_entries);
    print_entries(entries, count, show_inode);
    if (print_header)
        printf("\n");
    for (size_t i = 0; i < count; i++) {
        if (entries[i]) {
            free(entries[i]->name);
            if (entries[i]->link_target)
                free(entries[i]->link_target);
            free(entries[i]);
        }
    }
    free(entries);
}

void process_path(const char *path, int show_hidden, int print_header, FileEntry ***file_files, size_t *file_count, size_t *file_cap, int show_inode) {
    struct stat st;
    if (fstatat(AT_FDCWD, path, &st, AT_SYMLINK_NOFOLLOW) < 0)
        return;
    if (S_ISDIR(st.st_mode))
        process_directory(path, show_hidden, print_header, show_inode);
    else
        process_file_collect(path, file_files, file_count, file_cap);
}

int main(int argc, char *argv[]) {
    int show_hidden = 0, show_inode = 0, nonflag_count = 0;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-' && strlen(argv[i]) > 1) {
            size_t len = strlen(argv[i]);
            for (size_t j = 1; j < len; j++) {
                if (argv[i][j] == 'h')
                    show_hidden = 1;
                else if (argv[i][j] == 'i')
                    show_inode = 1;
                else {
                    fprintf(stderr, "Unknown flag: -%c\n", argv[i][j]);
                    return EXIT_FAILURE;
                }
            }
        } else {
            nonflag_count++;
        }
    }
    FileEntry **file_files = NULL;
    size_t file_count = 0, file_cap = 16;
    file_files = malloc(file_cap * sizeof(FileEntry *));
    if (!file_files)
        return EXIT_FAILURE;
    if (nonflag_count == 0) {
        process_directory(".", show_hidden, 0, show_inode);
    } else {
        int print_header = (nonflag_count > 1);
        for (int i = 1; i < argc; i++) {
            if (argv[i][0] == '-' && strlen(argv[i]) > 1)
                continue;
            glob_t results;
            int ret = glob(argv[i], 0, NULL, &results);
            if (ret != 0) {
                process_directory(argv[i], show_hidden, print_header, show_inode);
                process_file_collect(argv[i], &file_files, &file_count, &file_cap);
            } else {
                for (size_t j = 0; j < results.gl_pathc; j++) {
                    struct stat st;
                    if (fstatat(AT_FDCWD, results.gl_pathv[j], &st, AT_SYMLINK_NOFOLLOW) == 0 && S_ISDIR(st.st_mode))
                        process_directory(results.gl_pathv[j], show_hidden, print_header, show_inode);
                    else
                        process_file_collect(results.gl_pathv[j], &file_files, &file_count, &file_cap);
                }
            }
            globfree(&results);
        }
        if (file_count > 0) {
            qsort(file_files, file_count, sizeof(FileEntry *), cmp_entries);
            print_entries(file_files, file_count, show_inode);
            for (size_t i = 0; i < file_count; i++) {
                free(file_files[i]->name);
                if (file_files[i]->link_target)
                    free(file_files[i]->link_target);
                free(file_files[i]);
            }
            free(file_files);
        }
    }
    return EXIT_SUCCESS;
}
