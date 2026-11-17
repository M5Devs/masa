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
 * Does a single-string `system "..."` need a shell to interpret it?
 *
 * Ruby's Kernel#system runs a one-string command through `/bin/sh -c`,
 * so spaces separate words and metacharacters (`|`, `>`, `$`, `*`, ...)
 * take on their shell meaning.  A multi-argument `system "prog", "arg"`
 * runs the program directly instead.  A one-string command with neither
 * spaces nor metacharacters is a single bare word and is left on the
 * direct-exec path.
 */
static int command_needs_shell(const char *cmd)
{
    static const char *meta = "|&;<>()$`\\\"'*?[]{}~#!";
    size_t i;

    for (i = 0; cmd[i] != '\0'; i++) {
        if (cmd[i] == ' ' || cmd[i] == '\t' || cmd[i] == '\n') {
            return 1;
        }
        if (strchr(meta, cmd[i]) != NULL) {
            return 1;
        }
    }
    return 0;
}

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
                    /* A lone `system "..."` with spaces/metachars is a
                     * shell command (Ruby runs it via /bin/sh -c). */
                    out->steps[out->step_count].use_shell =
                        (count == 1 && command_needs_shell(argv[0]));
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
    printf("  install:  " MASA_SIZE_FMT " step%s\n",
           MASA_SIZE_ARG(f->step_count),
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

        if (step->use_shell) {
            /*
             * A single-string `system "..."` runs through the shell, so
             * spaces and metacharacters are interpreted.  Build
             * `<shell> -c "<command>"`, expanding the `#{...}`
             * interpolations inside the command string first.
             */
            char *expanded = expand_interp(step->argv[0], keg_prefix, keg_bin,
                                           version, f->name);

            if (expanded == NULL) {
                goto fail;
            }
            cmds[i] = calloc(4, sizeof(*cmds[i]));
            if (cmds[i] == NULL) {
                free(expanded);
                goto fail;
            }
            cmds[i][0] = strdup(masa_shell_path());
            cmds[i][1] = strdup("-c");
            cmds[i][2] = expanded;
            if (cmds[i][0] == NULL || cmds[i][1] == NULL) {
                /* Release this step now: the shared cleanup below stops at
                 * the first NULL slot and would miss the later entries. */
                free(cmds[i][0]);
                free(cmds[i][1]);
                free(cmds[i][2]);
                free(cmds[i]);
                cmds[i] = NULL;
                goto fail;
            }
            continue;
        }

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
    printf("masa: brew: running " MASA_SIZE_FMT " install step%s\n",
           MASA_SIZE_ARG(f.step_count),
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
/* Homebrew cask parser and linter                                    */
/* ------------------------------------------------------------------ */

/* Locate `cask "name" do` and return a fresh copy of the name, or NULL. */
static char *find_cask_token(const char *text)
{
    const char *p = text;

    while (*p != '\0') {
        const char *eol = strchr(p, '\n');
        size_t line_len = (eol != NULL) ? (size_t)(eol - p) : strlen(p);
        char *line = malloc(line_len + 1);
        char *tok = NULL;

        if (line == NULL) {
            return NULL;
        }
        memcpy(line, p, line_len);
        line[line_len] = '\0';

        {
            const char *s = ltrim(line);

            if (strncmp(s, "cask", 4) == 0 &&
                (s[4] == ' ' || s[4] == '\t')) {
                const char *q = s + 4;

                while (*q == ' ' || *q == '\t') {
                    q++;
                }
                tok = read_quoted(q, NULL);
            }
        }
        free(line);
        if (tok != NULL) {
            return tok;
        }
        if (eol == NULL) {
            break;
        }
        p = eol + 1;
    }
    return NULL;
}

void masa_brew_cask_free(struct masa_brew_cask *c)
{
    if (c == NULL) {
        return;
    }
    free(c->token);
    free(c->version);
    free(c->sha256);
    free(c->url);
    free(c->name);
    free(c->desc);
    free(c->homepage);
    free(c->app);
    memset(c, 0, sizeof(*c));
}

int masa_brew_cask_parse(const char *text, size_t len,
                         struct masa_brew_cask *out,
                         char *err, size_t errcap)
{
    const char *p;
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
            snprintf(err, errcap, "empty cask");
        }
        return -1;
    }

    out->token = find_cask_token(text);
    if (out->token == NULL) {
        if (err != NULL && errcap > 0) {
            snprintf(err, errcap, "no `cask \"<name>\" do` declaration");
        }
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

        if ((value = scalar_field(line, "version")) != NULL) {
            ok = (set_field(&out->version, value) == 0);
            free(value);
        } else if ((value = scalar_field(line, "sha256")) != NULL) {
            ok = (set_field(&out->sha256, value) == 0);
            free(value);
        } else if ((value = scalar_field(line, "url")) != NULL) {
            ok = (set_field(&out->url, value) == 0);
            free(value);
        } else if ((value = scalar_field(line, "name")) != NULL) {
            ok = (set_field(&out->name, value) == 0);
            free(value);
        } else if ((value = scalar_field(line, "desc")) != NULL) {
            ok = (set_field(&out->desc, value) == 0);
            free(value);
        } else if ((value = scalar_field(line, "homepage")) != NULL) {
            ok = (set_field(&out->homepage, value) == 0);
            free(value);
        } else if (!out->has_app &&
                   ((value = scalar_field(line, "app")) != NULL ||
                    (value = scalar_field(line, "pkg")) != NULL ||
                    (value = scalar_field(line, "binary")) != NULL)) {
            /* The artifact that lands in /Applications.  app/pkg/binary
             * are the three shapes a cask uses; the first one wins. */
            ok = (set_field(&out->app, value) == 0);
            out->has_app = 1;
            free(value);
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
        masa_brew_cask_free(out);
        if (err != NULL && errcap > 0) {
            snprintf(err, errcap, "out of memory");
        }
        return -1;
    }
    return 0;
}

int masa_brew_lint_cask(const struct masa_brew_cask *c, const char *label)
{
    const char *src = (label != NULL) ? label : "cask";
    int missing = 0;

    if (c->version == NULL || c->version[0] == '\0') {
        fprintf(stderr,
                "masa: brew lint: %s: missing required field 'version'\n", src);
        missing = 1;
    }
    if (c->sha256 == NULL || c->sha256[0] == '\0') {
        fprintf(stderr,
                "masa: brew lint: %s: missing required field 'sha256'\n", src);
        missing = 1;
    }
    if (c->url == NULL || c->url[0] == '\0') {
        fprintf(stderr, "masa: brew lint: %s: missing required field 'url'\n",
                src);
        missing = 1;
    }
    if (!c->has_app) {
        fprintf(stderr,
                "masa: brew lint: %s: missing required artifact "
                "(app/pkg/binary \"...\")\n", src);
        missing = 1;
    }
    if (c->homepage == NULL || c->homepage[0] == '\0') {
        fprintf(stderr,
                "masa: brew lint: %s: warning: no 'homepage'\n", src);
    }
    if (c->desc == NULL || c->desc[0] == '\0') {
        fprintf(stderr, "masa: brew lint: %s: warning: no 'desc'\n", src);
    }

    if (missing) {
        return 1;
    }

    printf("%s: ok (cask)\n", src);
    printf("  token:    %s\n", c->token);
    printf("  version:  %s\n", c->version);
    printf("  sha256:   %s\n", c->sha256);
    printf("  url:      %s\n", c->url);
    if (c->name != NULL) {
        printf("  name:     %s\n", c->name);
    }
    if (c->homepage != NULL) {
        printf("  homepage: %s\n", c->homepage);
    }
    if (c->desc != NULL) {
        printf("  desc:     %s\n", c->desc);
    }
    printf("  artifact: %s\n", c->app);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Online tap and fetch                                               */
/* ------------------------------------------------------------------ */

/* Case-insensitive ASCII equality (no strcasecmp dependency). */
static int str_ieq(const char *a, const char *b)
{
    while (*a != '\0' && *b != '\0') {
        int ca = tolower((unsigned char)*a);
        int cb = tolower((unsigned char)*b);

        if (ca != cb) {
            return 0;
        }
        a++;
        b++;
    }
    return *a == *b;
}

/* Copy src to dst in binary mode.  Returns 0 on success. */
static int copy_file(const char *src, const char *dst)
{
    FILE *in = fopen(src, "rb");
    FILE *out;
    unsigned char buf[65536];
    size_t got;

    if (in == NULL) {
        return -1;
    }
    out = fopen(dst, "wb");
    if (out == NULL) {
        fclose(in);
        return -1;
    }
    while ((got = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, got, out) != got) {
            fclose(in);
            fclose(out);
            return -1;
        }
    }
    if (ferror(in)) {
        fclose(in);
        fclose(out);
        return -1;
    }
    fclose(in);
    return (fclose(out) == 0) ? 0 : -1;
}

/* The file name a download URL resolves to, with any query stripped. */
static void url_basename(const char *url, char *buf, size_t cap)
{
    const char *slash = strrchr(url, '/');
    const char *base = (slash != NULL) ? slash + 1 : url;
    const char *q;

    if (base[0] == '\0') {
        base = "download";
    }
    q = strpbrk(base, "?#");
    if (q != NULL) {
        size_t n = (size_t)(q - base);

        if (n >= cap) {
            n = cap - 1;
        }
        memcpy(buf, base, n);
        buf[n] = '\0';
    } else {
        snprintf(buf, cap, "%s", base);
    }
}

int masa_brew_fetch_archive(const char *url, const char *expect_sha,
                            const char *out_dir, char *out_path,
                            size_t out_path_cap)
{
    char base[512];
    char path[MASA_PATH_MAX];
    char hex[65];

    if (url == NULL || url[0] == '\0') {
        fprintf(stderr, "masa: brew fetch: formula/cask has no 'url'\n");
        return -1;
    }
    if (out_dir == NULL || out_dir[0] == '\0') {
        out_dir = ".";
    }
    if (masa_mkdir_p(out_dir) != 0) {
        fprintf(stderr, "masa: brew fetch: cannot create '%s': %s\n", out_dir,
                strerror(errno));
        return -1;
    }
    url_basename(url, base, sizeof(base));
    if (snprintf(path, sizeof(path), "%s/%s", out_dir, base) >=
        (int)sizeof(path)) {
        fprintf(stderr, "masa: brew fetch: output path too long\n");
        return -1;
    }

    if (strncmp(url, "file://", 7) == 0) {
        /* A local archive: copy it, so `masa brew fetch` works offline. */
        const char *src = url + 7;

        if (copy_file(src, path) != 0) {
            fprintf(stderr, "masa: brew fetch: cannot copy '%s': %s\n", src,
                    strerror(errno));
            return -1;
        }
        printf("masa: brew: copied %s -> %s\n", src, path);
    } else {
        const char *argv[6];
        int rc;

        argv[0] = "curl";
        argv[1] = "-fSL";
        argv[2] = "-o";
        argv[3] = path;
        argv[4] = url;
        argv[5] = NULL;
        rc = masa_spawn_wait(argv);
        if (rc != 0) {
            fprintf(stderr,
                    "masa: brew fetch: curl failed for %s (status %d)\n", url,
                    rc);
            return -1;
        }
        printf("masa: brew: downloaded %s\n", url);
    }

    if (masa_sha256_file(path, hex) != 0) {
        fprintf(stderr, "masa: brew fetch: cannot hash '%s': %s\n", path,
                strerror(errno));
        return -1;
    }
    printf("masa: brew: sha256 %s\n", hex);
    printf("masa: brew: saved  %s\n", path);

    if (expect_sha == NULL || expect_sha[0] == '\0') {
        fprintf(stderr, "masa: brew fetch: no 'sha256' field to verify against\n");
        return -1;
    }
    if (!str_ieq(hex, expect_sha)) {
        fprintf(stderr,
                "masa: brew fetch: checksum mismatch\n"
                "masa: brew fetch:   expected %s\n"
                "masa: brew fetch:   actual   %s\n", expect_sha, hex);
        return 1;
    }
    printf("masa: brew: checksum OK\n");
    if (out_path != NULL && out_path_cap > 0) {
        snprintf(out_path, out_path_cap, "%s", path);
    }
    return 0;
}

/* The tap directory: $MASA_BREW_TAPS, else $HOME/.masa/brew/taps. */
static int brew_taps_root(char *buf, size_t cap)
{
    const char *env = getenv(MASA_BREW_TAPS_ENV);

    if (env != NULL && env[0] != '\0') {
        return (snprintf(buf, cap, "%s", env) >= (int)cap) ? -1 : 0;
    }
    {
        const char *home = getenv("HOME");

#ifdef _WIN32
        if (home == NULL || home[0] == '\0') {
            home = getenv("USERPROFILE");
        }
#endif
        if (home == NULL || home[0] == '\0') {
            home = "/tmp";
        }
        return (snprintf(buf, cap, "%s/.masa/brew/taps", home) >= (int)cap)
                   ? -1
                   : 0;
    }
}

/* Print every *.rb in dir, labelled formula or cask.  A missing dir is
 * not an error (a tap may ship only formulas, or only casks). */
static void list_tap_rbs(const char *dir)
{
    MASA_DIR *d = masa_opendir(dir);
    struct masa_dirent *ent;

    if (d == NULL) {
        return;
    }
    while ((ent = masa_readdir(d)) != NULL) {
        char full[MASA_PATH_MAX];
        char *text = NULL;
        size_t tlen = 0;
        char err[256];
        size_t n = strlen(ent->d_name);
        const char *kind;

        if (n < 3 || strcmp(ent->d_name + n - 3, ".rb") != 0) {
            continue;
        }
        if (snprintf(full, sizeof(full), "%s/%s", dir, ent->d_name) >=
            (int)sizeof(full)) {
            continue;
        }
        if (read_whole_file(full, &text, &tlen, err, sizeof(err)) != 0) {
            continue;
        }
        {
            char *tok = find_cask_token(text);

            kind = (tok != NULL) ? "cask" : "formula";
            free(tok);
        }
        printf("  %-7s %s\n", kind, ent->d_name);
        free(text);
    }
    masa_closedir(d);
}

/* The base URL taps are cloned from: $MASA_BREW_TAP_BASE, else github. */
static const char *tap_base(void)
{
    const char *env = getenv(MASA_BREW_TAP_BASE_ENV);

    if (env != NULL && env[0] != '\0') {
        return env;
    }
    return MASA_BREW_TAP_BASE_DEFAULT;
}

static int brew_tap(int argc, char *const argv[])
{
    const char *spec;
    const char *slash;
    size_t ulen;
    char user[256];
    char repo[256];
    char root[MASA_PATH_MAX];
    char dest[MASA_PATH_MAX];
    char url[MASA_PATH_MAX];
    char sub[MASA_PATH_MAX];

    if (argc < 1 || argv[0] == NULL || argv[0][0] == '\0') {
        fprintf(stderr, "masa: brew tap: missing <user/repo>\n");
        return 2;
    }
    spec = argv[0];
    slash = strchr(spec, '/');
    if (slash == NULL || slash == spec || slash[1] == '\0') {
        fprintf(stderr, "masa: brew tap: expected <user/repo>, got '%s'\n",
                spec);
        return 2;
    }
    ulen = (size_t)(slash - spec);
    if (ulen >= sizeof(user) || strlen(slash + 1) >= sizeof(repo)) {
        fprintf(stderr, "masa: brew tap: name too long\n");
        return 2;
    }
    memcpy(user, spec, ulen);
    user[ulen] = '\0';
    snprintf(repo, sizeof(repo), "%s", slash + 1);

    if (brew_taps_root(root, sizeof(root)) != 0 ||
        snprintf(dest, sizeof(dest), "%s/%s/%s", root, user, repo) >=
            (int)sizeof(dest) ||
        snprintf(url, sizeof(url), "%s/%s/homebrew-%s", tap_base(), user,
                 repo) >= (int)sizeof(url)) {
        fprintf(stderr, "masa: brew tap: path too long\n");
        return 1;
    }
    if (masa_mkdir_p(root) != 0) {
        fprintf(stderr, "masa: brew tap: cannot create '%s': %s\n", root,
                strerror(errno));
        return 1;
    }

    if (access(dest, F_OK) == 0) {
        const char *a[] = {"git", "-C", dest, "pull", "--ff-only", NULL};

        printf("masa: brew: updating tap %s/%s\n", user, repo);
        if (masa_spawn_wait(a) != 0) {
            fprintf(stderr, "masa: brew tap: `git pull` failed in %s\n", dest);
            return 1;
        }
    } else {
        const char *a[] = {"git", "clone", "--depth", "1", url, dest, NULL};

        printf("masa: brew: cloning %s\n", url);
        if (masa_spawn_wait(a) != 0) {
            fprintf(stderr, "masa: brew tap: `git clone` failed for %s\n", url);
            return 1;
        }
    }

    printf("masa: brew: tap %s/%s at %s\n", user, repo, dest);
    if (snprintf(sub, sizeof(sub), "%s/Formula", dest) < (int)sizeof(sub)) {
        list_tap_rbs(sub);
    }
    if (snprintf(sub, sizeof(sub), "%s/Casks", dest) < (int)sizeof(sub)) {
        list_tap_rbs(sub);
    }
    list_tap_rbs(dest);
    return 0;
}

int masa_brew_tap_main(int argc, char *const argv[])
{
    return brew_tap(argc, argv);
}

/* Decide whether a .rb document is a cask or a formula, fetch its URL
 * and verify the digest. */
static int fetch_from_rb(const char *text, size_t len, const char *label,
                         const char *out_dir)
{
    char err[512];
    struct masa_brew_cask c;
    struct masa_brew_formula f;
    int rc;

    if (masa_brew_cask_parse(text, len, &c, err, sizeof(err)) == 0) {
        printf("masa: brew fetch: cask %s\n", c.token);
        rc = masa_brew_fetch_archive(c.url, c.sha256, out_dir, NULL, 0);
        masa_brew_cask_free(&c);
        return (rc == 0) ? 0 : 1;
    }
    if (masa_brew_formula_parse(text, len, &f, err, sizeof(err)) == 0) {
        printf("masa: brew fetch: formula %s\n", f.name);
        rc = masa_brew_fetch_archive(f.url, f.sha256, out_dir, NULL, 0);
        masa_brew_formula_free(&f);
        return (rc == 0) ? 0 : 1;
    }
    fprintf(stderr, "masa: brew fetch: %s: not a formula or cask: %s\n", label,
            err);
    return 1;
}

static int brew_fetch(int argc, char *const argv[])
{
    const char *path = NULL;
    const char *out_dir = ".";
    char *text = NULL;
    size_t text_len = 0;
    char err[512];
    int rc;
    int i;

    for (i = 0; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 || strcmp(argv[i], "--output") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "masa: brew fetch: -o needs a directory\n");
                return 2;
            }
            out_dir = argv[++i];
        } else if (argv[i][0] == '-' && argv[i][1] != '\0') {
            fprintf(stderr, "masa: brew fetch: unknown option '%s'\n", argv[i]);
            return 2;
        } else if (path == NULL) {
            path = argv[i];
        } else {
            fprintf(stderr, "masa: brew fetch: unexpected argument '%s'\n",
                    argv[i]);
            return 2;
        }
    }

    if (path == NULL) {
        fprintf(stderr, "masa: brew fetch: missing <formula_or_cask.rb>\n");
        return 2;
    }
    if (read_whole_file(path, &text, &text_len, err, sizeof(err)) != 0) {
        fprintf(stderr, "masa: brew fetch: %s\n", err);
        return 1;
    }
    rc = fetch_from_rb(text, text_len, path, out_dir);
    free(text);
    return rc;
}

int masa_brew_fetch_main(int argc, char *const argv[])
{
    return brew_fetch(argc, argv);
}

/* ------------------------------------------------------------------ */
/* Online formula index: a minimal JSON scanner                       */
/* ------------------------------------------------------------------ */

/*
 * A cursor over a JSON document.  The scanner is deliberately tiny: it
 * decodes strings (with the standard escapes) and skips whole values, which
 * is all the Homebrew formula index needs.  It does not build a DOM, so a
 * multi-megabyte formula.json costs nothing to walk.
 */
struct json {
    const char *p;
    const char *end;
};

static void json_ws(struct json *j)
{
    while (j->p < j->end) {
        char c = *j->p;

        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            j->p++;
            continue;
        }
        break;
    }
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return 0;
}

static int json_utf8(uint32_t cp, char *out)
{
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/*
 * Decode the JSON string at j->p (which must be a '"'), returning a
 * malloc'd UTF-8 copy and advancing past the closing quote.  Returns NULL
 * on a malformed or unterminated string (leaving j->p unchanged when it
 * never saw an opening quote).
 */
static char *json_string(struct json *j)
{
    char *out;
    size_t cap;
    size_t len = 0;

    if (j->p >= j->end || *j->p != '"') {
        return NULL;
    }
    j->p++;
    cap = (size_t)(j->end - j->p) + 1;
    out = malloc(cap);
    if (out == NULL) {
        return NULL;
    }

    while (j->p < j->end) {
        char c = *j->p++;

        if (c == '"') {
            out[len] = '\0';
            return out;
        }
        if (c == '\\') {
            char e;

            if (j->p >= j->end) {
                break;
            }
            e = *j->p++;
            switch (e) {
            case '"':  out[len++] = '"';  break;
            case '\\': out[len++] = '\\'; break;
            case '/':  out[len++] = '/';  break;
            case 'b':  out[len++] = '\b'; break;
            case 'f':  out[len++] = '\f'; break;
            case 'n':  out[len++] = '\n'; break;
            case 'r':  out[len++] = '\r'; break;
            case 't':  out[len++] = '\t'; break;
            case 'u': {
                unsigned cp = 0;
                int k;

                for (k = 0; k < 4 && j->p < j->end; k++) {
                    cp = (cp << 4) | (unsigned)hexval(*j->p++);
                }
                if (cp >= 0xD800 && cp <= 0xDBFF && j->p + 1 < j->end &&
                    j->p[0] == '\\' && j->p[1] == 'u') {
                    unsigned lo = 0;

                    j->p += 2;
                    for (k = 0; k < 4 && j->p < j->end; k++) {
                        lo = (lo << 4) | (unsigned)hexval(*j->p++);
                    }
                    if (lo >= 0xDC00 && lo <= 0xDFFF) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                }
                len += (size_t)json_utf8(cp, out + len);
                break;
            }
            default:
                out[len++] = e;
                break;
            }
        } else {
            out[len++] = c;
        }
    }

    free(out);
    return NULL;
}

/* Skip one JSON value.  Returns 0 on success and -1 on a malformed doc. */
static int json_skip_value(struct json *j)
{
    char c;

    json_ws(j);
    if (j->p >= j->end) {
        return -1;
    }
    c = *j->p;

    if (c == '"') {
        char *s = json_string(j);

        if (s == NULL) {
            return -1;
        }
        free(s);
        return 0;
    }
    if (c == '{' || c == '[') {
        char open = c;
        char close = (c == '{') ? '}' : ']';
        int depth = 0;

        while (j->p < j->end) {
            char d = *j->p;

            if (d == '"') {
                char *s = json_string(j);

                if (s == NULL) {
                    return -1;
                }
                free(s);
                continue;
            }
            if (d == open) {
                depth++;
            } else if (d == close) {
                depth--;
                j->p++;
                if (depth == 0) {
                    return 0;
                }
                continue;
            }
            j->p++;
        }
        return -1;
    }

    /* number / true / false / null */
    while (j->p < j->end) {
        char d = *j->p;

        if (d == ',' || d == '}' || d == ']' || d == ' ' || d == '\t' ||
            d == '\n' || d == '\r') {
            break;
        }
        j->p++;
    }
    return 0;
}

void masa_brew_formula_info_free(struct masa_brew_formula_info *info)
{
    size_t i;

    if (info == NULL) {
        return;
    }
    free(info->name);
    free(info->version);
    free(info->desc);
    free(info->homepage);
    for (i = 0; i < info->dep_count; i++) {
        free(info->deps[i]);
    }
    free(info->deps);
    memset(info, 0, sizeof(*info));
}

/* Parse `"versions": { "stable": "1.2.3", ... }`, keeping only stable. */
static void json_parse_versions(struct json *j, struct masa_brew_formula_info *info)
{
    json_ws(j);
    if (j->p >= j->end || *j->p != '{') {
        json_skip_value(j);
        return;
    }
    j->p++;

    for (;;) {
        char *key;

        json_ws(j);
        if (j->p >= j->end || *j->p == '}') {
            if (j->p < j->end) {
                j->p++;
            }
            return;
        }
        if (*j->p == ',') {
            j->p++;
            continue;
        }
        key = json_string(j);
        if (key == NULL) {
            return;
        }
        json_ws(j);
        if (j->p < j->end && *j->p == ':') {
            j->p++;
        }
        json_ws(j);
        if (strcmp(key, "stable") == 0 && info->version == NULL) {
            char *v = json_string(j);

            if (v != NULL) {
                info->version = v;
            } else {
                json_skip_value(j);
            }
        } else {
            json_skip_value(j);
        }
        free(key);
    }
}

/* Parse `"dependencies": ["a", "b", ...]`. */
static void json_parse_deps(struct json *j, struct masa_brew_formula_info *info)
{
    json_ws(j);
    if (j->p >= j->end || *j->p != '[') {
        json_skip_value(j);
        return;
    }
    j->p++;

    for (;;) {
        char *v;

        json_ws(j);
        if (j->p >= j->end || *j->p == ']') {
            if (j->p < j->end) {
                j->p++;
            }
            return;
        }
        if (*j->p == ',') {
            j->p++;
            continue;
        }
        v = json_string(j);
        if (v == NULL) {
            json_skip_value(j);
            continue;
        }
        {
            char **grown = realloc(info->deps,
                                   (info->dep_count + 1) * sizeof(char *));

            if (grown != NULL) {
                info->deps = grown;
                info->deps[info->dep_count++] = v;
            } else {
                free(v);
            }
        }
    }
}

/*
 * Parse one formula object (j->p at the '{') into info, keeping only the
 * fields masa brew info/search need.  Returns 0 on success.
 */
static int json_parse_formula(struct json *j, struct masa_brew_formula_info *info)
{
    json_ws(j);
    if (j->p >= j->end || *j->p != '{') {
        return -1;
    }
    j->p++;

    for (;;) {
        char *key;

        json_ws(j);
        if (j->p >= j->end) {
            return -1;
        }
        if (*j->p == '}') {
            j->p++;
            return 0;
        }
        if (*j->p == ',') {
            j->p++;
            continue;
        }
        key = json_string(j);
        if (key == NULL) {
            return -1;
        }
        json_ws(j);
        if (j->p < j->end && *j->p == ':') {
            j->p++;
        }
        json_ws(j);
        if (strcmp(key, "name") == 0 || strcmp(key, "desc") == 0 ||
            strcmp(key, "homepage") == 0) {
            char *v = json_string(j);

            if (v != NULL) {
                if (strcmp(key, "name") == 0) {
                    free(info->name);
                    info->name = v;
                } else if (strcmp(key, "desc") == 0) {
                    free(info->desc);
                    info->desc = v;
                } else {
                    free(info->homepage);
                    info->homepage = v;
                }
            } else {
                json_skip_value(j);
            }
        } else if (strcmp(key, "versions") == 0) {
            json_parse_versions(j, info);
        } else if (strcmp(key, "dependencies") == 0) {
            json_parse_deps(j, info);
        } else {
            json_skip_value(j);
        }
        free(key);
    }
}

/* Case-insensitive "needle appears in hay". */
static int str_icontains(const char *hay, const char *needle)
{
    size_t nlen = strlen(needle);
    size_t hlen = strlen(hay);
    size_t i;

    if (nlen == 0) {
        return 1;
    }
    if (hlen < nlen) {
        return 0;
    }
    for (i = 0; i + nlen <= hlen; i++) {
        size_t k;

        for (k = 0; k < nlen; k++) {
            if (tolower((unsigned char)hay[i + k]) !=
                tolower((unsigned char)needle[k])) {
                break;
            }
        }
        if (k == nlen) {
            return 1;
        }
    }
    return 0;
}

/* Count the top-level objects in a formula.json array. */
static size_t json_count_objects(const char *text, size_t len)
{
    struct json j;
    size_t n = 0;

    j.p = text;
    j.end = text + len;
    json_ws(&j);
    if (j.p >= j.end || *j.p != '[') {
        return 0;
    }
    j.p++;
    for (;;) {
        json_ws(&j);
        if (j.p >= j.end || *j.p == ']') {
            break;
        }
        if (*j.p == ',') {
            j.p++;
            continue;
        }
        if (*j.p != '{') {
            break;
        }
        if (json_skip_value(&j) != 0) {
            break;
        }
        n++;
    }
    return n;
}

int masa_brew_info_lookup(const char *text, size_t len, const char *name,
                          struct masa_brew_formula_info *out)
{
    struct json j;

    if (out == NULL) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    j.p = text;
    j.end = text + len;
    json_ws(&j);
    if (j.p >= j.end || *j.p != '[') {
        return -1;
    }
    j.p++;

    for (;;) {
        struct masa_brew_formula_info info;

        json_ws(&j);
        if (j.p >= j.end || *j.p == ']') {
            return -1;
        }
        if (*j.p == ',') {
            j.p++;
            continue;
        }
        memset(&info, 0, sizeof(info));
        if (json_parse_formula(&j, &info) != 0) {
            masa_brew_formula_info_free(&info);
            return -1;
        }
        if (info.name != NULL && str_ieq(info.name, name)) {
            *out = info;
            return 0;
        }
        masa_brew_formula_info_free(&info);
    }
}

char **masa_brew_search(const char *text, size_t len, const char *query,
                        size_t *count)
{
    struct json j;
    char **names = NULL;
    size_t n = 0;

    if (count != NULL) {
        *count = 0;
    }
    j.p = text;
    j.end = text + len;
    json_ws(&j);
    if (j.p >= j.end || *j.p != '[') {
        return NULL;
    }
    j.p++;

    for (;;) {
        struct masa_brew_formula_info info;

        json_ws(&j);
        if (j.p >= j.end || *j.p == ']') {
            break;
        }
        if (*j.p == ',') {
            j.p++;
            continue;
        }
        memset(&info, 0, sizeof(info));
        if (json_parse_formula(&j, &info) != 0) {
            masa_brew_formula_info_free(&info);
            break;
        }
        if (info.name != NULL && str_icontains(info.name, query)) {
            char **grown = realloc(names, (n + 2) * sizeof(char *));

            if (grown != NULL) {
                names = grown;
                names[n] = strdup(info.name);
                if (names[n] != NULL) {
                    n++;
                }
                names[n] = NULL;
            }
        }
        masa_brew_formula_info_free(&info);
    }

    if (count != NULL) {
        *count = n;
    }
    return names;
}

/* The formula-index cache directory: $MASA_BREW_CACHE, else $HOME/.masa/brew. */
static int brew_cache_dir(char *buf, size_t cap)
{
    const char *env = getenv(MASA_BREW_CACHE_ENV);

    if (env != NULL && env[0] != '\0') {
        return (snprintf(buf, cap, "%s", env) >= (int)cap) ? -1 : 0;
    }
    {
        const char *home = getenv("HOME");

#ifdef _WIN32
        if (home == NULL || home[0] == '\0') {
            home = getenv("USERPROFILE");
        }
#endif
        if (home == NULL || home[0] == '\0') {
            home = "/tmp";
        }
        return (snprintf(buf, cap, "%s/.masa/brew", home) >= (int)cap) ? -1 : 0;
    }
}

static const char *brew_api_base(void)
{
    const char *env = getenv(MASA_BREW_API_BASE_ENV);

    if (env != NULL && env[0] != '\0') {
        return env;
    }
    return MASA_BREW_API_BASE_DEFAULT;
}

int masa_brew_update_main(int argc, char *const argv[])
{
    char cache[MASA_PATH_MAX];
    char dest[MASA_PATH_MAX];
    char src[MASA_PATH_MAX];
    const char *base;
    const char *local;
    const char *sep;

    (void)argc;
    (void)argv;

    if (brew_cache_dir(cache, sizeof(cache)) != 0) {
        fprintf(stderr, "masa: brew update: cache path too long\n");
        return 1;
    }
    if (masa_mkdir_p(cache) != 0) {
        fprintf(stderr, "masa: brew update: cannot create '%s': %s\n", cache,
                strerror(errno));
        return 1;
    }
    if (snprintf(dest, sizeof(dest), "%s/formula.json", cache) >=
        (int)sizeof(dest)) {
        fprintf(stderr, "masa: brew update: cache path too long\n");
        return 1;
    }

    base = brew_api_base();
    sep = (base[strlen(base) - 1] == '/') ? "" : "/";

    /*
     * A file:// URL or a bare path is read from the local filesystem, so
     * the index can be refreshed offline (a mirror, a cache, or a test
     * fixture); anything with a scheme is downloaded with curl.
     */
    if (strncmp(base, "file://", 7) == 0) {
        local = base + 7;
    } else if (strstr(base, "://") == NULL) {
        local = base;
    } else {
        local = NULL;
    }

    if (local != NULL) {
        if (snprintf(src, sizeof(src), "%s%sformula.json", local, sep) >=
            (int)sizeof(src)) {
            fprintf(stderr, "masa: brew update: source path too long\n");
            return 1;
        }
        if (copy_file(src, dest) != 0) {
            fprintf(stderr, "masa: brew update: cannot copy '%s': %s\n", src,
                    strerror(errno));
            return 1;
        }
        printf("masa: brew: copied %s -> %s\n", src, dest);
    } else {
        const char *curl[6];
        char url[MASA_PATH_MAX];
        int rc;

        if (snprintf(url, sizeof(url), "%s%sformula.json", base, sep) >=
            (int)sizeof(url)) {
            fprintf(stderr, "masa: brew update: URL too long\n");
            return 1;
        }
        curl[0] = "curl";
        curl[1] = "-fSL";
        curl[2] = "-o";
        curl[3] = dest;
        curl[4] = url;
        curl[5] = NULL;
        rc = masa_spawn_wait(curl);
        if (rc != 0) {
            fprintf(stderr,
                    "masa: brew update: curl failed for %s (status %d)\n", url,
                    rc);
            return 1;
        }
        printf("masa: brew: downloaded %s\n", url);
    }

    {
        char *text = NULL;
        size_t text_len = 0;
        char err[256];

        if (read_whole_file(dest, &text, &text_len, err, sizeof(err)) != 0) {
            fprintf(stderr, "masa: brew update: %s\n", err);
            return 1;
        }
        printf("masa: brew: updated %s (" MASA_SIZE_FMT " formulas)\n", dest,
               MASA_SIZE_ARG(json_count_objects(text, text_len)));
        free(text);
    }
    return 0;
}

/* Load the cached formula.json; report how to fetch it when missing. */
static int brew_load_index(char **out, size_t *out_len)
{
    char cache[MASA_PATH_MAX];
    char dest[MASA_PATH_MAX];
    char err[256];

    if (brew_cache_dir(cache, sizeof(cache)) != 0 ||
        snprintf(dest, sizeof(dest), "%s/formula.json", cache) >=
            (int)sizeof(dest)) {
        fprintf(stderr, "masa: brew: cache path too long\n");
        return -1;
    }
    if (read_whole_file(dest, out, out_len, err, sizeof(err)) != 0) {
        fprintf(stderr,
                "masa: brew: no cached index (%s); run `masa brew update`\n",
                dest);
        return -1;
    }
    return 0;
}

int masa_brew_info_main(int argc, char *const argv[])
{
    char *text = NULL;
    size_t text_len = 0;
    struct masa_brew_formula_info info;

    if (argc < 1 || argv[0] == NULL || argv[0][0] == '\0') {
        fprintf(stderr, "masa: brew info: missing <name>\n");
        return 2;
    }
    if (brew_load_index(&text, &text_len) != 0) {
        return 1;
    }
    if (masa_brew_info_lookup(text, text_len, argv[0], &info) != 0) {
        fprintf(stderr, "masa: brew info: no formula named '%s'\n", argv[0]);
        free(text);
        return 1;
    }
    free(text);

    printf("%s %s\n", info.name, (info.version != NULL) ? info.version : "");
    if (info.desc != NULL) {
        printf("%s\n", info.desc);
    }
    if (info.homepage != NULL) {
        printf("From: %s\n", info.homepage);
    }
    if (info.dep_count > 0) {
        size_t i;

        printf("Dependencies\n");
        for (i = 0; i < info.dep_count; i++) {
            printf("  %s\n", info.deps[i]);
        }
    }
    masa_brew_formula_info_free(&info);
    return 0;
}

int masa_brew_search_main(int argc, char *const argv[])
{
    char *text = NULL;
    size_t text_len = 0;
    char **names;
    size_t count = 0;
    size_t i;

    if (argc < 1 || argv[0] == NULL || argv[0][0] == '\0') {
        fprintf(stderr, "masa: brew search: missing <query>\n");
        return 2;
    }
    if (brew_load_index(&text, &text_len) != 0) {
        return 1;
    }
    names = masa_brew_search(text, text_len, argv[0], &count);
    free(text);
    if (names == NULL || count == 0) {
        printf("No formula found for '%s'\n", argv[0]);
        free(names);
        return 1;
    }
    for (i = 0; i < count; i++) {
        printf("%s\n", names[i]);
        free(names[i]);
    }
    free(names);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Entry point                                                        */
/* ------------------------------------------------------------------ */

/* Lint a .rb that may be a formula or a cask. */
static int lint_rb(const char *text, size_t len, const char *label)
{
    char err[512];
    struct masa_brew_cask c;
    struct masa_brew_formula f;
    int rc;

    if (masa_brew_cask_parse(text, len, &c, err, sizeof(err)) == 0) {
        rc = masa_brew_lint_cask(&c, label);
        masa_brew_cask_free(&c);
        return rc;
    }
    if (masa_brew_formula_parse(text, len, &f, err, sizeof(err)) == 0) {
        rc = masa_brew_lint_formula(&f, label);
        masa_brew_formula_free(&f);
        return rc;
    }
    fprintf(stderr, "masa: brew lint: %s: not a formula or cask: %s\n", label,
            err);
    return 1;
}

static int brew_lint(int argc, char *const argv[])
{
    const char *path;
    char *text = NULL;
    size_t text_len = 0;
    char err[512];
    int rc;

    if (argc < 1 || argv[0] == NULL || argv[0][0] == '\0') {
        fprintf(stderr, "masa: brew lint: missing <formula.rb|cask.rb>\n");
        return 2;
    }
    path = argv[0];

    if (read_whole_file(path, &text, &text_len, err, sizeof(err)) != 0) {
        fprintf(stderr, "masa: brew lint: %s\n", err);
        return 1;
    }
    rc = lint_rb(text, text_len, path);
    free(text);
    return rc;
}

int masa_brew_main(int argc, char *const argv[])
{
    if (argc < 1 || argv[0] == NULL || argv[0][0] == '\0') {
        fprintf(stderr,
                "masa: brew: missing subcommand "
                "(lint | build | tap | fetch)\n");
        return 2;
    }
    if (strcmp(argv[0], "lint") == 0) {
        return brew_lint(argc - 1, &argv[1]);
    }
    if (strcmp(argv[0], "build") == 0) {
        return brew_build(argc - 1, &argv[1]);
    }
    if (strcmp(argv[0], "tap") == 0) {
        return brew_tap(argc - 1, &argv[1]);
    }
    if (strcmp(argv[0], "fetch") == 0) {
        return brew_fetch(argc - 1, &argv[1]);
    }
    if (strcmp(argv[0], "update") == 0) {
        return masa_brew_update_main(argc - 1, &argv[1]);
    }
    if (strcmp(argv[0], "info") == 0) {
        return masa_brew_info_main(argc - 1, &argv[1]);
    }
    if (strcmp(argv[0], "search") == 0) {
        return masa_brew_search_main(argc - 1, &argv[1]);
    }

    fprintf(stderr, "masa: brew: unknown subcommand '%s'\n", argv[0]);
    return 2;
}

