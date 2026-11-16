/*
 * brew.c - a Homebrew formula linter and builder that needs no Ruby.
 *
 * `masa brew lint` reads a formula (.rb) and checks the fields a working
 * formula must have.  `masa brew build` goes further: it pulls the
 * `system` commands out of `def install`, lays out a Homebrew-style
 * Cellar, and runs the build steps inside Masa's shim environment so the
 * result is a real Mach-O executable.
 *
 * The formula reader is a small line-oriented scanner, not a Ruby
 * interpreter.  It understands the handful of constructs a formula uses
 * for metadata and install steps: the class declaration, the scalar
 * `key "value"` fields, and `system "prog", "arg", ...` calls, including
 * the `#{prefix}`, `#{bin}`, `#{version}` and `#{name}` interpolations.
 *
 * Copyright (C) 2026 Masa contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include "masa.h"
#include "masa_compat.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Small helpers                                                      */
/* ------------------------------------------------------------------ */

/* Replace *slot with a fresh copy of value (freeing the old one). */
static int set_field(char **slot, const char *value)
{
    char *copy = strdup(value);

    if (copy == NULL) {
        return -1;
    }
    free(*slot);
    *slot = copy;
    return 0;
}

/* Leading whitespace of a line, as a count of columns. */
static size_t indent_of(const char *line)
{
    size_t n = 0;

    while (line[n] == ' ' || line[n] == '\t') {
        n++;
    }
    return n;
}

/* Point at the first non-space character of a line. */
static const char *ltrim(const char *line)
{
    return line + indent_of(line);
}

/*
 * Read a double-quoted Ruby string starting at p (which must be the
 * opening quote) into a fresh buffer, unescaping \" and \\.  Returns the
 * position just past the closing quote, or NULL on a missing quote.
 */
static char *read_quoted(const char *p, const char **end)
{
    size_t cap;
    char *out;
    size_t n = 0;

    if (p == NULL || *p != '"') {
        return NULL;
    }
    cap = strlen(p) + 1;
    out = malloc(cap);
    if (out == NULL) {
        return NULL;
    }

    p++;
    while (*p != '\0' && *p != '"') {
        if (*p == '\\' && p[1] != '\0') {
            p++;
        }
        out[n++] = *p++;
    }
    if (*p != '"') {
        free(out);
        return NULL;
    }
    p++;
    out[n] = '\0';
    if (end != NULL) {
        *end = p;
    }
    return out;
}

/*
 * If the trimmed line is `key "..."`, return a fresh copy of the value.
 * Returns NULL when the line is not that field.
 */
static char *scalar_field(const char *line, const char *key)
{
    const char *p = ltrim(line);
    size_t klen = strlen(key);

    if (strncmp(p, key, klen) != 0) {
        return NULL;
    }
    p += klen;
    if (*p != ' ' && *p != '\t') {
        return NULL;
    }
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    return read_quoted(p, NULL);
}

/* ------------------------------------------------------------------ */
/* Formula name from the class name                                   */
/* ------------------------------------------------------------------ */

/*
 * Homebrew maps a CamelCase class to a kebab-case formula name:
 * `FooBar` -> "foo-bar", `FooBarBaz` -> "foo-bar-baz", `MyTool2` ->
 * "my-tool2".  A hyphen is inserted before an uppercase letter that
 * follows a lowercase letter or digit.
 */
static char *class_to_name(const char *cls)
{
    size_t len = strlen(cls);
    char *out = malloc(len * 2 + 1);
    size_t n = 0;
    size_t i;

    if (out == NULL) {
        return NULL;
    }

    for (i = 0; i < len; i++) {
        char c = cls[i];

        if (isupper((unsigned char)c) && i > 0 &&
            (islower((unsigned char)cls[i - 1]) ||
             isdigit((unsigned char)cls[i - 1]))) {
            out[n++] = '-';
        }
        out[n++] = (char)tolower((unsigned char)c);
    }
    out[n] = '\0';
    return out;
}

/* ------------------------------------------------------------------ */
/* Interpolation                                                      */
/* ------------------------------------------------------------------ */

/*
 * Expand the `#{...}` interpolations a formula's install steps use.  The
 * known keys are replaced; anything else is left verbatim so a formula
 * that needs Ruby-only helpers still shows what it asked for.
 */
static char *expand_interp(const char *s, const char *prefix, const char *bin,
                           const char *version, const char *name)
{
    size_t cap = strlen(s) + 1;
    size_t n = 0;
    char *out = malloc(cap);
    const char *p = s;

    if (out == NULL) {
        return NULL;
    }

    while (*p != '\0') {
        if (p[0] == '#' && p[1] == '{') {
            const char *close = strchr(p + 2, '}');

            if (close != NULL) {
                size_t klen = (size_t)(close - (p + 2));
                const char *value = NULL;

                if (klen == 6 && strncmp(p + 2, "prefix", 6) == 0) {
                    value = prefix;
                } else if (klen == 3 && strncmp(p + 2, "bin", 3) == 0) {
                    value = bin;
                } else if (klen == 7 && strncmp(p + 2, "version", 7) == 0) {
                    value = version;
                } else if (klen == 4 && strncmp(p + 2, "name", 4) == 0) {
                    value = name;
                }

                if (value != NULL) {
                    size_t vlen = strlen(value);

                    if (n + vlen + 1 > cap) {
                        size_t next = (n + vlen + 1) * 2;
                        char *grown = realloc(out, next);

                        if (grown == NULL) {
                            free(out);
                            return NULL;
                        }
                        out = grown;
                        cap = next;
                    }
                    memcpy(out + n, value, vlen);
                    n += vlen;
                    p = close + 1;
                    continue;
                }
            }
        }

        if (n + 2 > cap) {
            size_t next = cap * 2;
            char *grown = realloc(out, next);

            if (grown == NULL) {
                free(out);
                return NULL;
            }
            out = grown;
            cap = next;
        }
        out[n++] = *p++;
    }

    out[n] = '\0';
    return out;
}

/* ------------------------------------------------------------------ */
/* system "..." parsing                                               */
/* ------------------------------------------------------------------ */

/*
 * Split the argument list of a `system` call into tokens.  Each token is
 * either a double-quoted string or a bare word up to the next comma.
 * Returns a NULL-terminated array of fresh strings, or NULL on failure.
 */
static char **split_system_args(const char *p, size_t *out_count)
{
    char **items = NULL;
    size_t count = 0;
    size_t cap = 0;

    /* `system(...)` is equivalent to `system ...`; step past the paren. */
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (*p == '(') {
        p++;
    }

    for (;;) {
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (*p == '\0' || *p == '\n' || *p == '#' || *p == ')') {
            break;
        }

        {
            char *token = NULL;

            if (*p == '"') {
                const char *end = NULL;
                token = read_quoted(p, &end);
                if (token == NULL) {
                    break;
                }
                p = end;
            } else {
                const char *start = p;

                while (*p != '\0' && *p != ',' && *p != '\n' && *p != '#' &&
                       *p != ')') {
                    p++;
                }
                {
                    size_t len = (size_t)(p - start);
                    while (len > 0 &&
                           (start[len - 1] == ' ' || start[len - 1] == '\t')) {
                        len--;
                    }
                    token = malloc(len + 1);
                    if (token != NULL) {
                        memcpy(token, start, len);
                        token[len] = '\0';
                    }
                }
            }

            if (token == NULL) {
                break;
            }

            if (count + 2 > cap) {
                size_t next = (cap == 0) ? 4 : cap * 2;
                char **grown = realloc(items, next * sizeof(*grown));

                if (grown == NULL) {
                    free(token);
                    break;
                }
                items = grown;
                cap = next;
            }
            items[count++] = token;
        }

        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (*p == ',') {
            p++;
            continue;
        }
        break;
    }

    if (items == NULL) {
        *out_count = 0;
        return NULL;
    }
    items[count] = NULL;
    *out_count = count;
    return items;
}

static void free_step(struct masa_brew_step *step)
{
    size_t i;

    for (i = 0; i < step->argc; i++) {
        free(step->argv[i]);
    }
    free(step->argv);
    step->argv = NULL;
    step->prog = NULL;
    step->argc = 0;
}

/* ------------------------------------------------------------------ */
/* Parse                                                              */
/* ------------------------------------------------------------------ */

void masa_brew_formula_free(struct masa_brew_formula *f)
{
    size_t i;

    if (f == NULL) {
        return;
    }
    free(f->class_name);
    free(f->name);
    free(f->desc);
    free(f->homepage);
    free(f->url);
    free(f->sha256);
    free(f->license);
    free(f->version);
    for (i = 0; i < f->step_count; i++) {
        free_step(&f->steps[i]);
    }
    free(f->steps);
    memset(f, 0, sizeof(*f));
}

/*
 * Version derived from a URL, e.g. ".../mytool-1.2.3.tar.gz" -> "1.2.3".
 * A run of digits and dots counts as a version when it contains at least
 * one dot; trailing dots (as in "1.2.3.tar") are trimmed.  Returns a
 * fresh string or NULL when no version-looking token is found.
 */
static char *version_from_url(const char *url)
{
    const char *p = url;

    while (*p != '\0') {
        if (isdigit((unsigned char)*p)) {
            const char *start = p;
            int dots = 0;

            while (isdigit((unsigned char)*p) || *p == '.') {
                if (*p == '.') {
                    dots++;
                }
                p++;
            }
            if (dots >= 1) {
                size_t len = (size_t)(p - start);
                char *out;

                while (len > 0 && start[len - 1] == '.') {
                    len--;
                }
                out = malloc(len + 1);
                if (out == NULL) {
                    return NULL;
                }
                memcpy(out, start, len);
                out[len] = '\0';
                return out;
            }
            continue;
        }
        p++;
    }
    return NULL;
}

/*
 * Is this line the start of `def install`?  Matches `def install` with
 * optional arguments or a trailing comment.
 */
static int is_def_install(const char *line)
{
    const char *p = ltrim(line);

    if (strncmp(p, "def ", 4) != 0) {
        return 0;
    }
    p += 4;
    return strncmp(p, "install", 7) == 0 &&
           (p[7] == '\0' || p[7] == ' ' || p[7] == '\t' ||
            p[7] == '\n' || p[7] == '(');
}

static int is_plain_end(const char *line)
{
    const char *p = ltrim(line);

    return strncmp(p, "end", 3) == 0 &&
           (p[3] == '\0' || p[3] == ' ' || p[3] == '\t' || p[3] == '\n' ||
            p[3] == '#');
}

/*
 * Find `class <Name> < Formula` and return a fresh copy of <Name>.  The
 * `class` keyword, the name and the `<` superclass marker must all be
 * present; a line without a superclass is not a formula.
 */
static char *find_class(const char *text)
{
    const char *p = text;

    while (*p != '\0') {
        const char *eol = strchr(p, '\n');
        const char *line = p;

        if (strncmp(ltrim(line), "class ", 6) == 0) {
            const char *q = ltrim(line) + 6;
            const char *start = q;

            while (*q != '\0' && *q != '\n' && *q != ' ' && *q != '\t' &&
                   *q != '<') {
                q++;
            }
            if (q > start) {
                const char *rest = q;

                while (*rest == ' ' || *rest == '\t') {
                    rest++;
                }
                if (*rest == '<') {
                    size_t len = (size_t)(q - start);
                    char *out = malloc(len + 1);

                    if (out == NULL) {
                        return NULL;
                    }
                    memcpy(out, start, len);
                    out[len] = '\0';
                    return out;
                }
            }
        }

        if (eol == NULL) {
            break;
        }
        p = eol + 1;
    }
    return NULL;
}

/* Find `test do` anywhere in the text. */
static int find_test_block(const char *text)
{
    const char *p = text;

    while ((p = strstr(p, "test do")) != NULL) {
        return 1;
    }
    return 0;
}

int masa_brew_formula_parse(const char *text, size_t len,
                            struct masa_brew_formula *out,
                            char *err, size_t errcap)
{
    const char *p;
    int in_install = 0;
    size_t install_indent = 0;
    int ok = 1;

    if (err != NULL && errcap > 0) {
        err[0] = '\0';
    }
    if (out == NULL) {
        return -1;
    }
    memset(out, 0, sizeof(*out));

    if (text == NULL || len == 0) {
        if (err != NULL && errcap > 0) {
            snprintf(err, errcap, "empty formula");
        }
        return -1;
    }

    out->class_name = find_class(text);
    if (out->class_name == NULL) {
        if (err != NULL && errcap > 0) {
            snprintf(err, errcap, "no `class <Name> < Formula` declaration");
        }
        return -1;
    }
    out->name = class_to_name(out->class_name);
    if (out->name == NULL) {
        masa_brew_formula_free(out);
        return -1;
    }

    p = text;
    while (*p != '\0') {
        const char *eol = strchr(p, '\n');
        size_t line_len = (eol != NULL) ? (size_t)(eol - p) : strlen(p);
        char *line = malloc(line_len + 1);
        char *value;

        if (line == NULL) {
            ok = 0;
            break;
        }
        memcpy(line, p, line_len);
        line[line_len] = '\0';

        if (!in_install && is_def_install(line)) {
            in_install = 1;
            out->has_install = 1;
            install_indent = indent_of(line);
        } else if (in_install && is_plain_end(line) &&
                   indent_of(line) <= install_indent) {
            in_install = 0;
        } else if (in_install) {
            const char *s = ltrim(line);

            if (strncmp(s, "system", 6) == 0 &&
                (s[6] == ' ' || s[6] == '\t' || s[6] == '(')) {
                size_t count = 0;
                char **argv = split_system_args(s + 6, &count);

                if (argv != NULL && count > 0) {
                    struct masa_brew_step *grown =
                        realloc(out->steps,
                                (out->step_count + 1) * sizeof(*grown));

                    if (grown == NULL) {
                        size_t i;
                        for (i = 0; i < count; i++) {
                            free(argv[i]);
                        }
                        free(argv);
                        ok = 0;
                        free(line);
                        break;
                    }
                    out->steps = grown;
                    memset(&out->steps[out->step_count], 0,
                           sizeof(*out->steps));
                    out->steps[out->step_count].argv = argv;
                    out->steps[out->step_count].argc = count;
                    out->steps[out->step_count].prog = argv[0];
                    out->step_count++;
                } else {
                    free(argv);
                }
            }
        } else {
            if ((value = scalar_field(line, "desc")) != NULL) {
                ok = (set_field(&out->desc, value) == 0);
                free(value);
            } else if ((value = scalar_field(line, "homepage")) != NULL) {
                ok = (set_field(&out->homepage, value) == 0);
                free(value);
            } else if ((value = scalar_field(line, "url")) != NULL) {
                ok = (set_field(&out->url, value) == 0);
                free(value);
            } else if ((value = scalar_field(line, "sha256")) != NULL) {
                ok = (set_field(&out->sha256, value) == 0);
                free(value);
            } else if ((value = scalar_field(line, "license")) != NULL) {
                ok = (set_field(&out->license, value) == 0);
                free(value);
            } else if ((value = scalar_field(line, "version")) != NULL) {
                ok = (set_field(&out->version, value) == 0);
                free(value);
            }
        }

        free(line);
        if (!ok) {
            break;
        }
        if (eol == NULL) {
            break;
        }
        p = eol + 1;
    }

    if (!ok) {
        masa_brew_formula_free(out);
        if (err != NULL && errcap > 0) {
            snprintf(err, errcap, "out of memory");
        }
        return -1;
    }

    if (find_test_block(text)) {
        out->has_test = 1;
    }

    /* A formula without an explicit version derives one from its URL. */
    if (out->version == NULL && out->url != NULL) {
        out->version = version_from_url(out->url);
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* Lint                                                               */
/* ------------------------------------------------------------------ */

int masa_brew_lint_formula(const struct masa_brew_formula *f,
                           const char *label)
{
    const char *src = (label != NULL) ? label : "formula";
    int missing = 0;

    if (f->desc == NULL || f->desc[0] == '\0') {
        fprintf(stderr, "masa: brew lint: %s: missing required field 'desc'\n",
                src);
        missing = 1;
    }
    if (f->homepage == NULL || f->homepage[0] == '\0') {
        fprintf(stderr,
                "masa: brew lint: %s: missing required field 'homepage'\n", src);
        missing = 1;
    }
    if (f->url == NULL || f->url[0] == '\0') {
        fprintf(stderr, "masa: brew lint: %s: missing required field 'url'\n",
                src);
        missing = 1;
    }
    if (f->sha256 == NULL || f->sha256[0] == '\0') {
        fprintf(stderr,
                "masa: brew lint: %s: missing required field 'sha256'\n", src);
        missing = 1;
    }
    if (!f->has_install) {
        fprintf(stderr,
                "masa: brew lint: %s: missing required 'def install' block\n",
                src);
        missing = 1;
    }
    if (!f->has_test) {
        fprintf(stderr, "masa: brew lint: %s: warning: no 'test do' block\n",
                src);
    }

    if (missing) {
        return 1;
    }

    printf("%s: ok\n", src);
    printf("  name:     %s\n", f->name);
    printf("  version:  %s\n", (f->version != NULL) ? f->version : "(none)");
    printf("  desc:     %s\n", f->desc);
    printf("  homepage: %s\n", f->homepage);
    printf("  url:      %s\n", f->url);
    printf("  sha256:   %s\n", f->sha256);
    if (f->license != NULL) {
        printf("  license:  %s\n", f->license);
    }
    printf("  install:  %zu step%s\n", f->step_count,
           (f->step_count == 1) ? "" : "s");
    printf("  test:     %s\n", f->has_test ? "present" : "missing");
    return 0;
}

/* ------------------------------------------------------------------ */
/* Filesystem helpers                                                 */
/* ------------------------------------------------------------------ */

static int read_whole_file(const char *path, char **out, size_t *out_len,
                           char *err, size_t errcap)
{
    FILE *fp;
    char *buf;
    long size;
    size_t got;

    *out = NULL;
    *out_len = 0;

    fp = fopen(path, "rb");
    if (fp == NULL) {
        if (err != NULL && errcap > 0) {
            snprintf(err, errcap, "cannot open '%s': %s", path,
                     strerror(errno));
        }
        return -1;
    }
    if (fseek(fp, 0, SEEK_END) != 0 || (size = ftell(fp)) < 0 ||
        fseek(fp, 0, SEEK_SET) != 0) {
        if (err != NULL && errcap > 0) {
            snprintf(err, errcap, "cannot size '%s': %s", path,
                     strerror(errno));
        }
        fclose(fp);
        return -1;
    }

    buf = malloc((size_t)size + 1);
    if (buf == NULL) {
        fclose(fp);
        return -1;
    }
    got = fread(buf, 1, (size_t)size, fp);
    fclose(fp);
    buf[got] = '\0';
    *out = buf;
    *out_len = got;
    return 0;
}

static int is_macho_executable(const char *path, char err[256])
{
    static const unsigned char magic64[4] = {0xCF, 0xFA, 0xED, 0xFE};
    unsigned char header[16];
    FILE *fp;
    size_t got;

    err[0] = '\0';
    fp = fopen(path, "rb");
    if (fp == NULL) {
        snprintf(err, 256, "cannot read '%s'", path);
        return 0;
    }
    got = fread(header, 1, sizeof(header), fp);
    fclose(fp);
    if (got < sizeof(header)) {
        snprintf(err, 256, "'%s' is too small to be a Mach-O image", path);
        return 0;
    }
    if (memcmp(header, magic64, 4) != 0) {
        snprintf(err, 256, "'%s' is not a 64-bit Mach-O image", path);
        return 0;
    }
    if (header[12] != 0x02 || header[13] != 0 || header[14] != 0 ||
        header[15] != 0) {
        snprintf(err, 256, "'%s' is not an MH_EXECUTE image", path);
        return 0;
    }
    return 1;
}

/*
 * Create <prefix>/bin/<leaf> as a symlink to target, refreshing it if it
 * already exists.
 */
static int link_into_bin(const char *prefix, const char *leaf, const char *target)
{
    char bin_dir[MASA_PATH_MAX];
    char linkpath[MASA_PATH_MAX];

    if (snprintf(bin_dir, sizeof(bin_dir), "%s/bin", prefix) >=
            (int)sizeof(bin_dir) ||
        masa_mkdir_p(bin_dir) != 0) {
        return -1;
    }
    if (snprintf(linkpath, sizeof(linkpath), "%s/%s", bin_dir, leaf) >=
        (int)sizeof(linkpath)) {
        return -1;
    }
    return masa_symlink_or_copy(target, linkpath);
}

/* ------------------------------------------------------------------ */
/* Build                                                              */
/* ------------------------------------------------------------------ */

/* Resolve the version string a keg is filed under. */
static const char *formula_version(const struct masa_brew_formula *f)
{
    if (f->version != NULL && f->version[0] != '\0') {
        return f->version;
    }
    return "0.0.0";
}

/*
 * Run the formula's install steps inside the source directory with the
 * Masa shim environment, so `cc` and friends emit Mach-O.
 */
static int run_install_steps(const char *source_dir,
                             const struct masa_brew_formula *f,
                             const char *keg_prefix, const char *keg_bin,
                             const char *version)
{
    char ***cmds;
    size_t i;
    size_t j;
    int status;

    if (f->step_count == 0) {
        return 0;
    }

    cmds = calloc(f->step_count + 1, sizeof(*cmds));
    if (cmds == NULL) {
        return 1;
    }

    for (i = 0; i < f->step_count; i++) {
        const struct masa_brew_step *step = &f->steps[i];

        cmds[i] = calloc(step->argc + 1, sizeof(*cmds[i]));
        if (cmds[i] == NULL) {
            goto fail;
        }
        for (j = 0; j < step->argc; j++) {
            cmds[i][j] = expand_interp(step->argv[j], keg_prefix, keg_bin,
                                       version, f->name);
            if (cmds[i][j] == NULL) {
                goto fail;
            }
        }
    }

    status = masa_exec_run_all(source_dir, (char *const *const *)cmds,
                               f->step_count);

    for (i = 0; i < f->step_count; i++) {
        for (j = 0; cmds[i][j] != NULL; j++) {
            free(cmds[i][j]);
        }
        free(cmds[i]);
    }
    free(cmds);
    return status;

fail:
    for (i = 0; i < f->step_count; i++) {
        if (cmds[i] == NULL) {
            continue;
        }
        for (j = 0; cmds[i][j] != NULL; j++) {
            free(cmds[i][j]);
        }
        free(cmds[i]);
    }
    free(cmds);
    return 1;
}

static int brew_build(int argc, char *const argv[])
{
    const char *formula_path = NULL;
    const char *source_dir = NULL;
    const char *prefix = NULL;
    char *text = NULL;
    size_t text_len = 0;
    char err[512];
    struct masa_brew_formula f;
    const char *version;
    char keg[MASA_PATH_MAX];
    char keg_bin[MASA_PATH_MAX];
    MASA_DIR *dir;
    struct masa_dirent *ent;
    int installed = 0;
    int status;
    int i;

    for (i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--source") == 0 || strcmp(argv[i], "-s") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "masa: brew build: --source needs a directory\n");
                return 2;
            }
            source_dir = argv[++i];
        } else if (strcmp(argv[i], "--prefix") == 0 || strcmp(argv[i], "-p") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "masa: brew build: --prefix needs a directory\n");
                return 2;
            }
            prefix = argv[++i];
        } else if (argv[i][0] == '-' && argv[i][1] != '\0') {
            fprintf(stderr, "masa: brew build: unknown option '%s'\n", argv[i]);
            return 2;
        } else if (formula_path == NULL) {
            formula_path = argv[i];
        } else {
            fprintf(stderr, "masa: brew build: unexpected argument '%s'\n",
                    argv[i]);
            return 2;
        }
    }

    if (formula_path == NULL) {
        fprintf(stderr, "masa: brew build: missing <formula.rb>\n");
        return 2;
    }
    if (source_dir == NULL) {
        fprintf(stderr, "masa: brew build: missing --source <dir>\n");
        return 2;
    }

    if (prefix == NULL) {
        const char *env_prefix = getenv(MASA_BREW_PREFIX_ENV);

        if (env_prefix != NULL && env_prefix[0] != '\0') {
            prefix = env_prefix;
        } else {
            /* Fall back under $TMPDIR so Android/Termux and sandboxes can
             * redirect the Cellar away from an unwritable /tmp. */
            const char *tmpdir = getenv(MASA_TMPDIR_ENV);

            if (tmpdir != NULL && tmpdir[0] != '\0') {
                static char defbuf[MASA_PATH_MAX];

                snprintf(defbuf, sizeof(defbuf), "%s/masa-homebrew", tmpdir);
                prefix = defbuf;
            } else {
                prefix = MASA_BREW_DEFAULT_PREFIX;
            }
        }
    }

    {
        struct stat st;

        if (stat(source_dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
            fprintf(stderr, "masa: brew build: source '%s' is not a directory\n",
                    source_dir);
            return 1;
        }
    }

    if (read_whole_file(formula_path, &text, &text_len, err, sizeof(err)) != 0) {
        fprintf(stderr, "masa: brew build: %s\n", err);
        return 1;
    }
    if (masa_brew_formula_parse(text, text_len, &f, err, sizeof(err)) != 0) {
        fprintf(stderr, "masa: brew build: malformed formula: %s\n", err);
        free(text);
        return 1;
    }
    free(text);

    if (!f.has_install) {
        fprintf(stderr,
                "masa: brew build: formula has no 'def install' block\n");
        masa_brew_formula_free(&f);
        return 1;
    }

    version = formula_version(&f);

    if (snprintf(keg, sizeof(keg), "%s/Cellar/%s/%s", prefix, f.name,
                 version) >= (int)sizeof(keg) ||
        snprintf(keg_bin, sizeof(keg_bin), "%s/bin", keg) >=
            (int)sizeof(keg_bin)) {
        fprintf(stderr, "masa: brew build: prefix path too long\n");
        masa_brew_formula_free(&f);
        return 1;
    }

    if (masa_mkdir_p(keg_bin) != 0) {
        fprintf(stderr, "masa: brew build: cannot create '%s': %s\n", keg_bin,
                strerror(errno));
        masa_brew_formula_free(&f);
        return 1;
    }

    printf("masa: brew: %s %s\n", f.name, version);
    printf("masa: brew: source %s\n", source_dir);
    printf("masa: brew: keg    %s\n", keg);
    printf("masa: brew: running %zu install step%s\n", f.step_count,
           (f.step_count == 1) ? "" : "s");

    status = run_install_steps(source_dir, &f, keg, keg_bin, version);
    if (status != 0) {
        fprintf(stderr, "masa: brew build: install step failed (status %d)\n",
                status);
        masa_brew_formula_free(&f);
        return 1;
    }

    /* Verify every binary in the keg and link it into <prefix>/bin. */
    dir = masa_opendir(keg_bin);
    if (dir == NULL) {
        fprintf(stderr, "masa: brew build: cannot read '%s': %s\n", keg_bin,
                strerror(errno));
        masa_brew_formula_free(&f);
        return 1;
    }

    while ((ent = masa_readdir(dir)) != NULL) {
        char full[MASA_PATH_MAX];
        struct stat st;
        char reason[256];

        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        if (snprintf(full, sizeof(full), "%s/%s", keg_bin, ent->d_name) >=
            (int)sizeof(full)) {
            continue;
        }
        if (stat(full, &st) != 0 || !S_ISREG(st.st_mode)) {
            continue;
        }

        if (!is_macho_executable(full, reason)) {
            fprintf(stderr, "masa: brew build: %s\n", reason);
            continue;
        }

        printf("masa: brew: installed %s (Mach-O MH_EXECUTE)\n", full);
        if (link_into_bin(prefix, ent->d_name, full) != 0) {
            fprintf(stderr, "masa: brew build: cannot link %s into %s/bin\n",
                    ent->d_name, prefix);
            continue;
        }
        printf("masa: brew: linked    %s/bin/%s -> %s\n", prefix, ent->d_name,
               full);
        installed++;
    }
    masa_closedir(dir);

    if (installed == 0) {
        fprintf(stderr,
                "masa: brew build: no Mach-O executable was installed in %s\n",
                keg_bin);
        masa_brew_formula_free(&f);
        return 1;
    }

    printf("masa: brew: done, %d binary%s installed\n", installed,
           (installed == 1) ? "" : "ies");
    masa_brew_formula_free(&f);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Entry point                                                        */
/* ------------------------------------------------------------------ */

static int brew_lint(int argc, char *const argv[])
{
    const char *path;
    char *text = NULL;
    size_t text_len = 0;
    char err[512];
    struct masa_brew_formula f;
    int rc;

    if (argc < 1 || argv[0] == NULL || argv[0][0] == '\0') {
        fprintf(stderr, "masa: brew lint: missing <formula.rb>\n");
        return 2;
    }
    path = argv[0];

    if (read_whole_file(path, &text, &text_len, err, sizeof(err)) != 0) {
        fprintf(stderr, "masa: brew lint: %s\n", err);
        return 1;
    }
    if (masa_brew_formula_parse(text, text_len, &f, err, sizeof(err)) != 0) {
        fprintf(stderr, "masa: brew lint: %s: %s\n", path, err);
        free(text);
        return 1;
    }
    free(text);

    rc = masa_brew_lint_formula(&f, path);
    masa_brew_formula_free(&f);
    return rc;
}

int masa_brew_main(int argc, char *const argv[])
{
    if (argc < 1 || argv[0] == NULL || argv[0][0] == '\0') {
        fprintf(stderr, "masa: brew: missing subcommand (lint | build)\n");
        return 2;
    }
    if (strcmp(argv[0], "lint") == 0) {
        return brew_lint(argc - 1, &argv[1]);
    }
    if (strcmp(argv[0], "build") == 0) {
        return brew_build(argc - 1, &argv[1]);
    }

    fprintf(stderr, "masa: brew: unknown subcommand '%s'\n", argv[0]);
    return 2;
}
