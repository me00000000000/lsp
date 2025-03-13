#define _GNU_SOURCE
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
#define COLOR_GREEN "\033[32m"
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
#define THREAD_POOL_SIZE 4

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
    char size_str[BUF_SIZE];
    char time_str[BUF_SIZE];
} FileEntry;

int opt_sort_by_size = 0;
int opt_sort_by_name = 0;
int opt_reverse_sort = 0;

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

    int result = 0;
    if (opt_sort_by_name) {
        result = strcmp(fa->name, fb->name);
    } else if (!fa->is_dir && opt_sort_by_size) {
        if (fa->size < fb->size)
            result = 1;
        else if (fa->size > fb->size)
            result = -1;
        else
            result = 0;
    } else {
        if (fa->mtime < fb->mtime)
            result = 1;
        else if (fa->mtime > fb->mtime)
            result = -1;
        else
            result = 0;
    }

    if (opt_reverse_sort)
        result = -result;
    return result;
}

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
        if (entry->d_type != DT_UNKNOWN) {
            if (entry->d_type == DT_DIR)
                total += get_directory_size(full);
            else {
                struct stat st;
                if (stat(full, &st) == 0)
                    total += st.st_size;
            }
        } else {
            struct stat st;
            if (stat(full, &st) == 0) {
                if (S_ISDIR(st.st_mode))
                    total += get_directory_size(full);
                else
                    total += st.st_size;
            }
        }
    }
    closedir(d);
    return total;
}

typedef struct UidCache {
    uid_t uid;
    char username[256];
    struct UidCache *next;
} UidCache;
static UidCache *uid_cache = NULL;

const char* get_username_cached(uid_t uid) {
    UidCache *cur = uid_cache;
    while (cur) {
        if (cur->uid == uid)
            return cur->username;
        cur = cur->next;
    }
    struct passwd pwd;
    struct passwd *result = NULL;
    char buf[1024];
    if (getpwuid_r(uid, &pwd, buf, sizeof(buf), &result) == 0 && result) {
        UidCache *new_cache = malloc(sizeof(UidCache));
        if (new_cache) {
            new_cache->uid = uid;
            strncpy(new_cache->username, pwd.pw_name, sizeof(new_cache->username) - 1);
            new_cache->username[sizeof(new_cache->username) - 1] = '\0';
            new_cache->next = uid_cache;
            uid_cache = new_cache;
            return new_cache->username;
        }
    }
    return "unknown";
}

void free_uid_cache() {
    UidCache *cur = uid_cache;
    while (cur) {
        UidCache *next = cur->next;
        free(cur);
        cur = next;
    }
    uid_cache = NULL;
}

typedef struct GidCache {
    gid_t gid;
    char groupname[256];
    struct GidCache *next;
} GidCache;
static GidCache *gid_cache = NULL;

const char* get_groupname_cached(gid_t gid) {
    GidCache *cur = gid_cache;
    while (cur) {
        if (cur->gid == gid)
            return cur->groupname;
        cur = cur->next;
    }
    struct group grp;
    struct group *result = NULL;
    char buf[1024];
    if (getgrgid_r(gid, &grp, buf, sizeof(buf), &result) == 0 && result) {
        GidCache *new_cache = malloc(sizeof(GidCache));
        if (new_cache) {
            new_cache->gid = gid;
            strncpy(new_cache->groupname, grp.gr_name, sizeof(new_cache->groupname)-1);
            new_cache->groupname[sizeof(new_cache->groupname)-1] = '\0';
            new_cache->next = gid_cache;
            gid_cache = new_cache;
            return new_cache->groupname;
        }
    }
    return "unknown";
}

void free_gid_cache() {
    GidCache *cur = gid_cache;
    while (cur) {
        GidCache *next = cur->next;
        free(cur);
        cur = next;
    }
    gid_cache = NULL;
}

typedef struct Task {
    void (*function)(void *);
    void *arg;
    struct Task *next;
} Task;

typedef struct ThreadPool {
    pthread_t *threads;
    int num_threads;
    Task *task_queue_head;
    Task *task_queue_tail;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    int stop;
    int tasks_pending;
    pthread_cond_t tasks_done;
} ThreadPool;

static void *thread_pool_worker(void *arg) {
    ThreadPool *pool = (ThreadPool *)arg;
    while (1) {
        pthread_mutex_lock(&pool->lock);
        while (!pool->task_queue_head && !pool->stop)
            pthread_cond_wait(&pool->cond, &pool->lock);
        if (pool->stop && !pool->task_queue_head) {
            pthread_mutex_unlock(&pool->lock);
            break;
        }
        Task *task = pool->task_queue_head;
        pool->task_queue_head = task->next;
        if (!pool->task_queue_head)
            pool->task_queue_tail = NULL;
        pthread_mutex_unlock(&pool->lock);
        
        task->function(task->arg);
        free(task);
        
        pthread_mutex_lock(&pool->lock);
        pool->tasks_pending--;
        if (pool->tasks_pending == 0 && pool->task_queue_head == NULL)
            pthread_cond_signal(&pool->tasks_done);
        pthread_mutex_unlock(&pool->lock);
    }
    return NULL;
}

ThreadPool *thread_pool_create(int num_threads) {
    ThreadPool *pool = malloc(sizeof(ThreadPool));
    if (!pool)
        return NULL;
    pool->num_threads = num_threads;
    pool->stop = 0;
    pool->tasks_pending = 0;
    pool->task_queue_head = pool->task_queue_tail = NULL;
    pthread_mutex_init(&pool->lock, NULL);
    pthread_cond_init(&pool->cond, NULL);
    pthread_cond_init(&pool->tasks_done, NULL);
    pool->threads = malloc(num_threads * sizeof(pthread_t));
    if (!pool->threads) {
        free(pool);
        return NULL;
    }
    for (int i = 0; i < num_threads; i++) {
        if (pthread_create(&pool->threads[i], NULL, thread_pool_worker, pool) != 0) {
            free(pool->threads);
            free(pool);
            return NULL;
        }
    }
    return pool;
}

void thread_pool_add_task(ThreadPool *pool, void (*function)(void *), void *arg) {
    Task *task = malloc(sizeof(Task));
    if (!task)
        return;
    task->function = function;
    task->arg = arg;
    task->next = NULL;
    pthread_mutex_lock(&pool->lock);
    if (pool->task_queue_tail)
        pool->task_queue_tail->next = task;
    else
        pool->task_queue_head = task;
    pool->task_queue_tail = task;
    pool->tasks_pending++;
    pthread_cond_signal(&pool->cond);
    pthread_mutex_unlock(&pool->lock);
}

void thread_pool_wait(ThreadPool *pool) {
    pthread_mutex_lock(&pool->lock);
    while (pool->tasks_pending || pool->task_queue_head)
        pthread_cond_wait(&pool->tasks_done, &pool->lock);
    pthread_mutex_unlock(&pool->lock);
}

void thread_pool_destroy(ThreadPool *pool) {
    pthread_mutex_lock(&pool->lock);
    pool->stop = 1;
    pthread_cond_broadcast(&pool->cond);
    pthread_mutex_unlock(&pool->lock);
    for (int i = 0; i < pool->num_threads; i++)
        pthread_join(pool->threads[i], NULL);
    free(pool->threads);
    pthread_mutex_destroy(&pool->lock);
    pthread_cond_destroy(&pool->cond);
    pthread_cond_destroy(&pool->tasks_done);
    free(pool);
}

FileEntry *populate_file_entry(const char *name, const char *fullpath, const struct stat *st, time_t now) {
    FileEntry *fe = malloc(sizeof(FileEntry));
    if (!fe)
        return NULL;
    fe->name = strdup(name);
    strncpy(fe->fullpath, fullpath, PATH_MAX - 1);
    fe->fullpath[PATH_MAX - 1] = '\0';
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
    human_readable_size(fe->size, fe->size_str, sizeof(fe->size_str));
    time_ago(fe->mtime, now, fe->time_str, sizeof(fe->time_str));
    return fe;
}

FileEntry* create_file_entry_with_now(const char *filepath, time_t now) {
    struct stat st;
    if (fstatat(AT_FDCWD, filepath, &st, AT_SYMLINK_NOFOLLOW) < 0)
        return NULL;
    return populate_file_entry(filepath, filepath, &st, now);
}

void process_file_collect(const char *filepath, FileEntry ***files, size_t *count, size_t *cap) {
    time_t now = time(NULL);
    FileEntry *fe = create_file_entry_with_now(filepath, now);
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

typedef struct {
    int dirfd;
    char dirpath[PATH_MAX];
    char dname[NAME_MAX + 1];
    FileEntry **result;
    time_t now;
} ThreadTaskArg;

void process_entry_task(void *arg) {
    ThreadTaskArg *tta = (ThreadTaskArg *)arg;
    char full[PATH_MAX];
    snprintf(full, PATH_MAX, "%s/%s", tta->dirpath, tta->dname);
    struct stat st;
    if (fstatat(tta->dirfd, tta->dname, &st, AT_SYMLINK_NOFOLLOW) < 0)
        *(tta->result) = NULL;
    else {
        FileEntry *fe = malloc(sizeof(FileEntry));
        if (fe) {
            fe->name = strdup(tta->dname);
            strncpy(fe->fullpath, full, PATH_MAX - 1);
            fe->fullpath[PATH_MAX - 1] = '\0';
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
            human_readable_size(fe->size, fe->size_str, sizeof(fe->size_str));
            time_ago(fe->mtime, tta->now, fe->time_str, sizeof(fe->time_str));
            *(tta->result) = fe;
        } else
            *(tta->result) = NULL;
    }
    free(tta);
}

void print_entries(FileEntry **entries, size_t count, int show_inode) {
    int max_perm = 0, max_user = 0, max_size = 0, max_date = 0, max_inode = 0, max_nlink = 0;
    size_t max_name = 0;
    char line[1024];
    for (size_t i = 0; i < count; i++) {
        FileEntry *fe = entries[i];
        char perms[11];
        get_permission_string(fe->mode, perms);
        int len = strlen(perms);
        if (len > max_perm)
            max_perm = len;
        char usergroup[64];
        const char *username = get_username_cached(fe->uid);
        const char *groupname = get_groupname_cached(fe->gid);
        snprintf(usergroup, sizeof(usergroup), "%s:%s", username, groupname);
        len = strlen(usergroup);
        if (len > max_user)
            max_user = len;
        len = strlen(fe->size_str);
        if (len > max_size)
            max_size = len;
        len = strlen(fe->time_str);
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
    for (size_t i = 0; i < count; i++) {
        FileEntry *fe = entries[i];
        char perms[11];
        get_permission_string(fe->mode, perms);
        const char *username = get_username_cached(fe->uid);
        const char *groupname = get_groupname_cached(fe->gid);
        char usergroup[64];
        snprintf(usergroup, sizeof(usergroup), "%s:%s", username, groupname);
        const char *size_color = "";
        if (strstr(fe->size_str, "KB"))
            size_color = COLOR_GREEN;
        else if (strstr(fe->size_str, "MB"))
            size_color = COLOR_ORANGE;
        else if (strstr(fe->size_str, "GB"))
            size_color = COLOR_RED;
        const char *date_color = "";
        time_t now = time(NULL);
        double seconds = difftime(now, fe->mtime);
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
            pos += snprintf(line + pos, sizeof(line) - pos, "%s  ", inode_str);
            snprintf(inode_str, sizeof(inode_str), "%-*lu", max_nlink, (unsigned long)fe->nlink);
            pos += snprintf(line + pos, sizeof(line) - pos, "%s  ", inode_str);
        }
        pos += snprintf(line + pos, sizeof(line) - pos, "%-*s  %-*s  %s%-*s%s  %s%-*s%s  ",
                        max_perm, perms,
                        max_user, usergroup,
                        size_color, max_size, fe->size_str, COLOR_RESET,
                        date_color, max_date, fe->time_str, COLOR_RESET);
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
            pos += snprintf(line + pos, sizeof(line) - pos, "%s%s%s -> %s%s%s", name_color, fe->name, COLOR_RESET, target_color, fe->link_target, COLOR_RESET);
            if (is_char)
                pos += snprintf(line + pos, sizeof(line) - pos, "%s*%s", COLOR_RED, COLOR_RESET);
            else if (is_block)
                pos += snprintf(line + pos, sizeof(line) - pos, "%s#%s", COLOR_YELLOW, COLOR_RESET);
        } else {
            pos += snprintf(line + pos, sizeof(line) - pos, "%s%s%s", name_color, fe->name, COLOR_RESET);
        }
        if (S_ISCHR(fe->mode))
            pos += snprintf(line + pos, sizeof(line) - pos, "%s*%s", COLOR_RED, COLOR_RESET);
        else if (S_ISBLK(fe->mode))
            pos += snprintf(line + pos, sizeof(line) - pos, "%s#%s", COLOR_YELLOW, COLOR_RESET);
        puts(line);
    }
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
            free(namelist[i]);
        }
        free(namelist);
        return;
    }
    FileEntry **entries = malloc(n * sizeof(FileEntry *));
    size_t count = 0;
    time_t now = time(NULL);
    int use_thread_pool = (n >= THREAD_THRESHOLD);
    ThreadPool *pool = NULL;
    if (use_thread_pool)
        pool = thread_pool_create(THREAD_POOL_SIZE);
    for (int i = 0; i < n; i++) {
        if (!namelist[i])
            continue;
        if (!show_hidden && namelist[i]->d_name[0] == '.') {
            free(namelist[i]);
            continue;
        }
        if (use_thread_pool) {
            ThreadTaskArg *tta = malloc(sizeof(ThreadTaskArg));
            if (!tta) {
                char full[PATH_MAX];
                snprintf(full, PATH_MAX, "%s/%s", dirpath, namelist[i]->d_name);
                struct stat st;
                if (fstatat(dirfd, namelist[i]->d_name, &st, AT_SYMLINK_NOFOLLOW) == 0) {
                    FileEntry *fe = populate_file_entry(namelist[i]->d_name, full, &st, now);
                    entries[count++] = fe;
                }
                free(namelist[i]);
                continue;
            }
            tta->dirfd = dirfd;
            strncpy(tta->dirpath, dirpath, PATH_MAX - 1);
            tta->dirpath[PATH_MAX - 1] = '\0';
            strncpy(tta->dname, namelist[i]->d_name, NAME_MAX);
            tta->dname[NAME_MAX] = '\0';
            tta->result = &entries[count];
            tta->now = now;
            thread_pool_add_task(pool, process_entry_task, tta);
            count++;
        } else {
            char full[PATH_MAX];
            snprintf(full, PATH_MAX, "%s/%s", dirpath, namelist[i]->d_name);
            struct stat st;
            if (fstatat(dirfd, namelist[i]->d_name, &st, AT_SYMLINK_NOFOLLOW) < 0) {
                free(namelist[i]);
                continue;
            }
            FileEntry *fe = populate_file_entry(namelist[i]->d_name, full, &st, now);
            entries[count++] = fe;
        }
        free(namelist[i]);
    }

    free(namelist);
    if (use_thread_pool) {
        thread_pool_wait(pool);
        thread_pool_destroy(pool);
    }
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

void process_path(const char *path, int show_hidden, int print_header,
                  FileEntry ***file_files, size_t *file_count, size_t *file_cap, int show_inode) {
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
                else if (argv[i][j] == 's')
                    opt_sort_by_size = 1;
                else if (argv[i][j] == 'n')
                    opt_sort_by_name = 1;
                else if (argv[i][j] == 'r')
                    opt_reverse_sort = 1;
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
                    if (fstatat(AT_FDCWD, results.gl_pathv[j], &st, AT_SYMLINK_NOFOLLOW) == 0 &&
                        S_ISDIR(st.st_mode))
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
    free_uid_cache();
    free_gid_cache();
    return EXIT_SUCCESS;
}
