/*
 * quirkd — демон quirk-политик CactOS.
 *
 * В ядре quirk-и применяются внутри драйверов (ACPI/XHCI/таймеры). Этот демон
 * добавляет юзерспейс-слой: по правилам из /etc/quirkd.conf следит за devfs и
 * помечает/игнорирует узлы, а также пишет диагностику в журнал.
 *
 * Формат правил (/etc/quirkd.conf), по одному на строку:
 *   quirk=<подстрока имени узла>:<action>
 *   action: report | ignore   (по умолчанию report)
 *
 * Пример:
 *   quirk=tty:report
 *   quirk=fb0:ignore
 *
 * report — событие появления узла логируется и печатается;
 * ignore  — появление узла логируется как «подавлено» и не печатается.
 *
 * Запускается супервизором cgoct как /sbin/quirkd.
 *
 * /etc/quirkd.conf (необязательные ключи):
 *   file=/var/log/quirkd.log
 *   console=0
 *   interval=5
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>

#define MAX_DEV   96
#define MAX_NAME  64
#define MAX_QUIRK 16
#define MAX_RULE  96

static char log_path[128] = "/var/log/quirkd.log";
static int  console_on    = 0;
static int  interval_sec  = 5;
static int  out_fd        = -1;

enum quirk_action { Q_REPORT = 0, Q_IGNORE = 1 };

struct rule {
    char substr[MAX_NAME];
    enum quirk_action action;
};

static struct rule rules[MAX_QUIRK];
static int           rules_n = 0;

struct snapshot {
    int  n;
    char name[MAX_DEV][MAX_NAME];
};

static struct snapshot prev_snap;

static void config_load(void) {
    FILE *f = fopen("/etc/quirkd.conf", "r");
    if (!f) return;
    char line[192];
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || *p == '\0') continue;
        char *eq = p;
        while (*eq && *eq != '=' && *eq != '\n') eq++;
        if (*eq != '=') continue;
        *eq = '\0';
        char *key = p;
        char *val = eq + 1;
        int vlen = (int)strlen(val);
        while (vlen > 0 && (val[vlen - 1] == '\n' || val[vlen - 1] == '\r' ||
                            val[vlen - 1] == ' ' || val[vlen - 1] == '\t'))
            val[--vlen] = '\0';

        if (strcmp(key, "file") == 0) {
            strncpy(log_path, val, sizeof(log_path) - 1);
            log_path[sizeof(log_path) - 1] = '\0';
        } else if (strcmp(key, "console") == 0) {
            console_on = (val[0] == '1' || val[0] == 'y' || val[0] == 'Y');
        } else if (strcmp(key, "interval") == 0) {
            int v = atoi(val);
            if (v >= 1 && v <= 3600) interval_sec = v;
        } else if (strcmp(key, "quirk") == 0) {
            /* quirk=<substr>:<action> */
            char *colon = strchr(val, ':');
            if (!colon) continue;
            *colon = '\0';
            if (rules_n < MAX_QUIRK && val[0] != '\0') {
                strncpy(rules[rules_n].substr, val, sizeof(rules[rules_n].substr) - 1);
                rules[rules_n].substr[sizeof(rules[rules_n].substr) - 1] = '\0';
                rules[rules_n].action =
                    (strcmp(colon + 1, "ignore") == 0) ? Q_IGNORE : Q_REPORT;
                rules_n++;
            }
        }
    }
    fclose(f);
}

static void log_event(const char *msg) {
    if (out_fd >= 0) {
        write(out_fd, msg, strlen(msg));
    }
    if (console_on) {
        int cfd = open("/dev/console", O_WRONLY);
        if (cfd >= 0) {
            write(cfd, msg, strlen(msg));
            close(cfd);
        }
    }
}

static int name_in(const struct snapshot *s, const char *name) {
    int i;
    for (i = 0; i < s->n; i++) {
        if (strcmp(s->name[i], name) == 0) return 1;
    }
    return 0;
}

static int match_rule(const char *name, struct rule *out) {
    int i;
    for (i = 0; i < rules_n; i++) {
        if (strstr(name, rules[i].substr)) {
            *out = rules[i];
            return 1;
        }
    }
    return 0;
}

static void scan_devfs(struct snapshot *s) {
    s->n = 0;
    int fd = open("/dev", O_RDONLY);
    if (fd < 0) return;
    struct dirent buf[24];
    int n;
    while ((n = getdents(fd, buf, sizeof(buf))) > 0) {
        int count = n / (int)sizeof(struct dirent);
        int i;
        for (i = 0; i < count && s->n < MAX_DEV; i++) {
            const char *nm = buf[i].d_name;
            if (nm[0] == '\0') continue;
            if (strcmp(nm, ".") == 0 || strcmp(nm, "..") == 0) continue;
            strncpy(s->name[s->n], nm, MAX_NAME - 1);
            s->name[s->n][MAX_NAME - 1] = '\0';
            s->n++;
        }
    }
    close(fd);
}

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    printf("quirkd: starting\n");
    config_load();

    out_fd = open(log_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (out_fd < 0) {
        printf("quirkd: cannot open %s\n", log_path);
    }
    log_event("quirkd: starting\n");
    if (rules_n == 0) {
        log_event("quirkd: no quirks configured\n");
    }

    scan_devfs(&prev_snap);

    for (;;) {
        struct snapshot cur;
        char line[MAX_NAME + 96];
        int i;

        scan_devfs(&cur);

        for (i = 0; i < cur.n; i++) {
            if (name_in(&prev_snap, cur.name[i])) continue;

            struct rule r;
            if (match_rule(cur.name[i], &r)) {
                if (r.action == Q_REPORT) {
                    snprintf(line, sizeof(line),
                             "quirkd: device %s matched quirk '%s' (report)\n",
                             cur.name[i], r.substr);
                    printf("%s", line);
                } else {
                    snprintf(line, sizeof(line),
                             "quirkd: device %s suppressed by quirk '%s' (ignore)\n",
                             cur.name[i], r.substr);
                }
                log_event(line);
            }
        }

        prev_snap = cur;
        sleep((unsigned int)interval_sec);
    }
    return 0;
}
